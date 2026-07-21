/* =========================================================================
 * nwcusbap.sys — IN-KERNEL 802.11 SoftAP MAC responder
 *
 * Ported VERBATIM (logic) from native-windows/nwcusb_probe.c, converted to
 * kernel-C. This section drops INTO native-windows/kmdf/nwcusbap.c. See the
 * integration checklist at the bottom of this file.
 *
 * DESIGN (per the kernel constraints):
 *   - NwcEvtBulkInComplete (IRQL <= DISPATCH) copies the RX frame into the
 *     existing Rx ring (unchanged) and additionally KeSetEvent(&RxEvent).
 *   - A dedicated PsCreateSystemThread system thread (NwcResponderThread) runs
 *     at PASSIVE_LEVEL, waits on {RxEvent, StopEvent}, drains the ring under
 *     RxLock (copy-out only; lock released before any TX), parses each frame
 *     and, for probe-req / auth-seq1 / auth-seq3 / assoc-req addressed to us,
 *     builds the response and sends it with WdfUsbTargetPipeWriteSynchronously
 *     (legal at PASSIVE). All TX is serialized through TxLock (WDFWAITLOCK) so
 *     the beacon timer and the responder never collide on the single half-
 *     duplex bulk-OUT TX engine (mirrors the user-mode single-threaded loop).
 *   - Beacon: a PASSIVE WDF timer (~100 ms) calls the same in-kernel send path.
 *
 * No user-mode APIs, no float, no getenv (the proven defaults are baked as
 * NWC_* policy constants below). Requires linking ntstrsafe.lib (RtlStringCbA).
 * ========================================================================= */

/* (SSID always supplied by user mode via SET_APCONFIG; no in-kernel generator,
 *  so no ntstrsafe.lib dependency.) */

/* ---- Debug gate: 0 = silent (default). Set 1 to enable sparse DbgPrint. ---- */
#define NWC_DBG 0
#if NWC_DBG
#define NWC_LOG(...) DbgPrint("nwcusbap: " __VA_ARGS__)
#else
#define NWC_LOG(...) ((void)0)
#endif

/* ---- Baked policy (was runtime env in user mode; proven values only) ------ */
/* nwcusb_probe.c send_80211_frame:988-992  — RETRY_LIMIT stays 0 (NWC_RETRY7 off) */
/* nwcusb_probe.c should_answer_probe:1228   — NWCUSBAP-only probe filter (NWC_ALLPROBE off) */
/* nwcusb_probe.c auth_answered_recently:1302 — dedup ON (NWC_NODEDUP off) */
#define NWC_REG_GRANT_MS     2000ULL   /* reg_is_accepted_now:1159  pending->accept after ~2s */
#define NWC_AUTH_DEDUP_MS     800ULL   /* auth_answered_recently:1305 same (src,seq) window   */
#define NWC_AUTH_SLOT_FREE_MS 4000ULL  /* auth_answered_recently:1309 slot-reuse age          */
#define NWC_BEACON_PERIOD_MS  100      /* build_beacon:874 beacon interval / timer cadence     */

/* =========================================================================
 * DEVICE_CONTEXT ADDITIONS
 * Add these fields to the existing struct _DEVICE_CONTEXT in nwcusbap.c.
 * (Reproduced here as a struct only for review; DO NOT define a 2nd struct —
 *  paste the fields into the real DEVICE_CONTEXT.)
 * ========================================================================= */
#if 0  /* ---- paste the body into DEVICE_CONTEXT, then delete this #if 0 ---- */
    /* --- responder operating parameters (handed down by IOCTL_NWC_SET_APCONFIG) --- */
    UCHAR              Mac[6];              /* our AP/BSSID MAC */
    CHAR               Ssid[33];            /* connector SSID (may be 32 non-NUL bytes) */
    ULONG              SsidLen;             /* 0..32 */
    UCHAR              Channel;             /* 1..14 */
    BOOLEAN            Privacy;             /* WEP on */
    volatile LONG      Configured;          /* 0 until SET_APCONFIG arrives */

    /* --- responder thread + wakeup --- */
    PVOID              ThreadObj;           /* referenced ETHREAD for the join */
    KEVENT             RxEvent;             /* SynchronizationEvent: RX enqueued */
    KEVENT             StopEvent;           /* NotificationEvent: teardown */
    volatile LONG      Stopping;            /* belt-and-suspenders stop flag */

    /* --- TX serialization (uncontended: single responder thread is sole TX caller) --- */
    WDFWAITLOCK        TxLock;              /* held across guardian+frame OUT (PASSIVE) */
    UCHAR              TxScratch[20 + 512 + 8];  /* TXD+frame, used only under TxLock */

    /* --- responder-thread-private scratch (single consumer => no lock) --- */
    UCHAR              RxWork[RX_FRAME_MAX];     /* one drained frame */
    UCHAR              WepDecrypt[256];          /* wep_decrypt_body plaintext */
    UCHAR              AuthPlain[192];           /* decrypted auth body */

    /* --- per-station tables (responder-thread-private) --- */
    struct { UCHAR mac[6]; ULONGLONG firstMs; LONG used; } Reg[16];      /* reg_is_accepted_now */
    struct { UCHAR mac[6]; USHORT seq; ULONGLONG ms; LONG used; } Auth[16]; /* auth_answered_recently */
#endif

/* Forward decls (mutual references, mirrors user-mode file order). */
static int  NwcMgmtIeOffset(const UCHAR *frame, int len);
static VOID NwcMaybeAnswerManagement(PDEVICE_CONTEXT ctx, const UCHAR *frame, int len);

/* =========================================================================
 * Byte helpers  (nwcusb_probe.c put16/put32/get16/get32 : 138-163)
 * ========================================================================= */
static __forceinline VOID   NwcPut16(UCHAR *p, USHORT v){ p[0]=(UCHAR)v; p[1]=(UCHAR)(v>>8); }
static __forceinline VOID   NwcPut32(UCHAR *p, ULONG v){ p[0]=(UCHAR)v; p[1]=(UCHAR)(v>>8); p[2]=(UCHAR)(v>>16); p[3]=(UCHAR)(v>>24); }
static __forceinline USHORT NwcGet16(const UCHAR *p){ return (USHORT)(p[0] | ((USHORT)p[1]<<8)); }
static __forceinline ULONG  NwcGet32(const UCHAR *p){ return (ULONG)p[0] | ((ULONG)p[1]<<8) | ((ULONG)p[2]<<16) | ((ULONG)p[3]<<24); }

static __forceinline BOOLEAN NwcMacEqual(const UCHAR a[6], const UCHAR b[6]){ return (BOOLEAN)RtlEqualMemory(a,b,6); }
static __forceinline BOOLEAN NwcMacBroadcast(const UCHAR a[6]){
    static const UCHAR bc[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    return (BOOLEAN)RtlEqualMemory(a,bc,6);
}

/* Monotonic milliseconds. Replaces user-mode GetTickCount(). Safe at any IRQL. */
static __forceinline ULONGLONG NwcNowMs(void){ return (ULONGLONG)(KeQueryInterruptTime() / 10000ULL); }

/* =========================================================================
 * CRYPTO  (all pure byte math — ported verbatim)
 * ========================================================================= */

/* nwcusb_probe.c crc32_ieee : 755-764 */
static ULONG NwcCrc32Ieee(const UCHAR *data, int len)
{
    ULONG crc = 0xffffffffu;
    int i, bit;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* nwcusb_probe.c rc4_crypt : 766-791 */
static VOID NwcRc4Crypt(const UCHAR *key, int key_len, const UCHAR *in, UCHAR *out, int len)
{
    UCHAR s[256];
    UCHAR i = 0, j = 0, tmp;
    int n;
    for (n = 0; n < 256; n++) s[n] = (UCHAR)n;
    for (n = 0; n < 256; n++) {
        j = (UCHAR)(j + s[n] + key[n % key_len]);
        tmp = s[n]; s[n] = s[j]; s[j] = tmp;
    }
    i = 0; j = 0;
    for (n = 0; n < len; n++) {
        i = (UCHAR)(i + 1);
        j = (UCHAR)(j + s[i]);
        tmp = s[i]; s[i] = s[j]; s[j] = tmp;
        out[n] = (UCHAR)(in[n] ^ s[(UCHAR)(s[i] + s[j])]);
    }
}

/* nwcusb_probe.c wep_decrypt_body : 793-829.
 * `decrypted` moved to caller-supplied scratch (ctx->WepDecrypt) to keep the
 * responder thread stack shallow. */
static BOOLEAN NwcWepDecryptBody(const UCHAR *body, int body_len, const UCHAR key[13],
                                 UCHAR *decrypted /*>=256*/, UCHAR *plain, int plain_cap, int *plain_len)
{
    UCHAR rc4_key[16];
    int crypt_len, data_len;
    ULONG got_icv, calc_icv;

    *plain_len = 0;
    if (body_len < 8) return FALSE;
    crypt_len = body_len - 4;
    if (crypt_len > 256 || crypt_len < 4) return FALSE;

    rc4_key[0] = body[0]; rc4_key[1] = body[1]; rc4_key[2] = body[2];
    RtlCopyMemory(rc4_key + 3, key, 13);
    NwcRc4Crypt(rc4_key, sizeof(rc4_key), body + 4, decrypted, crypt_len);

    data_len = crypt_len - 4;
    got_icv  = NwcGet32(decrypted + data_len);
    calc_icv = NwcCrc32Ieee(decrypted, data_len);
    if (got_icv != calc_icv) return FALSE;
    if (data_len > plain_cap) return FALSE;
    RtlCopyMemory(plain, decrypted, data_len);
    *plain_len = data_len;
    return TRUE;
}

/* nwcusb_probe.c derive_original_wep_key : 696-747  (pure byte math, verbatim) */
static VOID NwcDeriveOriginalWepKey(const UCHAR ssid[/*>=20*/], UCHAR key[13])
{
    static const UCHAR subst[16] = { 0x0a,0x0d,0x0e,0x08,0x09,0x03,0x06,0x00,
                                     0x0c,0x05,0x02,0x07,0x0b,0x01,0x0f,0x04 };
    static const UCHAR perm[13]  = { 0x05,0x01,0x0c,0x04,0x02,0x03,0x0a,
                                     0x00,0x0b,0x07,0x09,0x08,0x06 };
    static const UCHAR mask_a[13]= { 'g','w','i','\'','6','&','f','s','=','0','N','f','~' };
    static const UCHAR mask_b[13]= { '%','(','e','g','E','r',')','a','g','(','s','&','m' };
    UCHAR tmp[13];
    int i;

    for (i = 0; i < 13; i++) key[i] = ssid[i] ^ ssid[13 + (i % 7)];
    for (i = 0; i < 7;  i++) key[3 + i] ^= ssid[13 + i];
    for (i = 0; i < 13; i++) key[i] ^= mask_a[i];

    RtlCopyMemory(tmp, key, sizeof(tmp));
    for (i = 0; i < 13; i++) key[perm[i]] = tmp[i];

    for (i = 0; i < 13; i++) {
        key[i] ^= mask_b[i];
        key[i] = (UCHAR)((subst[key[i] >> 4] << 4) | subst[key[i] & 0x0f]);
    }
    key[0] ^= key[6];  key[3] ^= key[9];  key[6] = key[3] ^ key[6];  key[9]  ^= key[0];  key[12] ^= key[0];
    key[1] ^= key[7];  key[4] ^= key[10]; key[7] = key[4] ^ key[7];  key[10] ^= key[1];  key[12] ^= key[1];
    key[2] ^= key[8];  key[5] ^= key[11]; key[8] = key[5] ^ key[8];  key[11] ^= key[2];  key[12] ^= key[2];
}

/* (in-kernel SSID generator removed — user mode supplies the SSID via SET_APCONFIG) */

/* =========================================================================
 * FRAME BUILDERS  (write the raw 802.11 frame; caller wraps with the TXD)
 * ssid passed explicitly with length to avoid strlen in kernel.
 * ========================================================================= */

/* nwcusb_probe.c build_beacon : 857-889 */
static size_t NwcBuildBeacon(UCHAR *out, size_t cap, const UCHAR mac[6],
                             const CHAR *ssid, ULONG ssid_len, UCHAR channel, BOOLEAN privacy)
{
    UCHAR *p = out;
    if (ssid_len > 32) ssid_len = 32;
    if (cap < 128) return 0;
    NwcPut16(p,0x0080); p+=2;                 /* beacon */
    NwcPut16(p,0x0000); p+=2;                 /* duration */
    RtlFillMemory(p,6,0xff); p+=6;            /* DA broadcast */
    RtlCopyMemory(p,mac,6); p+=6;             /* SA */
    RtlCopyMemory(p,mac,6); p+=6;             /* BSSID */
    NwcPut16(p,0x0000); p+=2;                 /* seq */
    RtlZeroMemory(p,8); p+=8;                 /* timestamp */
    NwcPut16(p,100); p+=2;                    /* beacon interval */
    NwcPut16(p, privacy?0x0011:0x0001); p+=2; /* ESS (+Privacy) */
    *p++=0; *p++=(UCHAR)ssid_len; RtlCopyMemory(p,ssid,ssid_len); p+=ssid_len;
    *p++=1; *p++=4; *p++=0x82; *p++=0x84; *p++=0x0b; *p++=0x16;  /* rates */
    *p++=3; *p++=1; *p++=channel;                                 /* DS param */
    *p++=5; *p++=4; *p++=0; *p++=1; *p++=0; *p++=0;               /* TIM */
    return (size_t)(p - out);
}

/* nwcusb_probe.c build_probe_response : 891-921 */
static size_t NwcBuildProbeResponse(UCHAR *out, size_t cap, const UCHAR mac[6], const UCHAR dst[6],
                                    const CHAR *ssid, ULONG ssid_len, UCHAR channel, BOOLEAN privacy)
{
    UCHAR *p = out;
    if (ssid_len > 32) ssid_len = 32;
    if (cap < 128) return 0;
    NwcPut16(p,0x0050); p+=2;                 /* probe response */
    NwcPut16(p,0x0000); p+=2;
    RtlCopyMemory(p,dst,6); p+=6;
    RtlCopyMemory(p,mac,6); p+=6;
    RtlCopyMemory(p,mac,6); p+=6;
    NwcPut16(p,0x0000); p+=2;
    RtlZeroMemory(p,8); p+=8;
    NwcPut16(p,100); p+=2;
    NwcPut16(p, privacy?0x0011:0x0001); p+=2;
    *p++=0; *p++=(UCHAR)ssid_len; RtlCopyMemory(p,ssid,ssid_len); p+=ssid_len;
    *p++=1; *p++=4; *p++=0x82; *p++=0x84; *p++=0x0b; *p++=0x16;
    *p++=3; *p++=1; *p++=channel;
    return (size_t)(p - out);
}

/* nwcusb_probe.c build_auth_response : 923-948 */
static size_t NwcBuildAuthResponse(UCHAR *out, size_t cap, const UCHAR mac[6], const UCHAR dst[6],
                                   USHORT alg, USHORT seq, USHORT status, BOOLEAN include_challenge)
{
    UCHAR *p = out; UCHAR i;
    if (cap < 160) return 0;
    NwcPut16(p,0x00b0); p+=2;                 /* authentication */
    NwcPut16(p,0x0000); p+=2;
    RtlCopyMemory(p,dst,6); p+=6;
    RtlCopyMemory(p,mac,6); p+=6;
    RtlCopyMemory(p,mac,6); p+=6;
    NwcPut16(p,0x0000); p+=2;
    NwcPut16(p,alg); p+=2;
    NwcPut16(p,seq); p+=2;
    NwcPut16(p,status); p+=2;
    if (include_challenge) {
        *p++=16; *p++=128;
        for (i = 0; i < 128; i++) *p++ = (UCHAR)(0xa5u ^ i);
    }
    return (size_t)(p - out);
}

/* nwcusb_probe.c build_assoc_response : 950-972 */
static size_t NwcBuildAssocResponse(UCHAR *out, size_t cap, const UCHAR mac[6], const UCHAR dst[6],
                                    USHORT aid, BOOLEAN reassoc, BOOLEAN privacy)
{
    UCHAR *p = out;
    if (cap < 48) return 0;
    NwcPut16(p, reassoc?0x0030:0x0010); p+=2;
    NwcPut16(p,0x0000); p+=2;
    RtlCopyMemory(p,dst,6); p+=6;
    RtlCopyMemory(p,mac,6); p+=6;
    RtlCopyMemory(p,mac,6); p+=6;
    NwcPut16(p,0x0000); p+=2;
    NwcPut16(p, privacy?0x0011:0x0001); p+=2;
    NwcPut16(p,0x0000); p+=2;                 /* status success */
    NwcPut16(p,(USHORT)(0xc000 | (aid & 0x3fff))); p+=2;
    *p++=1; *p++=4; *p++=0x82; *p++=0x84; *p++=0x0b; *p++=0x16;
    return (size_t)(p - out);
}

/* =========================================================================
 * TX PATH  (nwcusb_probe.c send_80211_frame : 974-1079)
 * Builds the 20-byte TXD (w0/w1/w2, words 3/4 = IV = 0), then the
 * guardian-then-frame bulk-OUT pair. Serialized via TxLock. Uses ctx->TxScratch
 * so nothing large lives on the caller stack.  request_ack/request_timestamp
 * carry the same meaning as user mode.
 * ========================================================================= */
static NTSTATUS NwcSend80211Frame(PDEVICE_CONTEXT ctx, const UCHAR *frame, size_t frame_len,
                                  BOOLEAN request_timestamp, BOOLEAN request_ack)
{
    ULONG w0 = 0, w1 = 0, w2, data_len, plcp_duration;
    int transfer_len;
    UCHAR guardian = 0;
    WDF_MEMORY_DESCRIPTOR md;
    ULONG sent = 0;
    NTSTATUS st;

    if (frame_len > 512) return STATUS_INVALID_PARAMETER;

    /* --- TXD word 0 --- (retry_limit baked 0; NWC_RETRY7 dropped) */
    if (request_ack)       w0 |= 0x00000200u;   /* TXD_W0_ACK */
    if (request_timestamp) w0 |= 0x00000400u;   /* TXD_W0_TIMESTAMP */
    w0 |= 0x00001000u;                          /* TXD_W0_NEW_SEQ */
    w0 |= ((ULONG)frame_len & 0x0fff) << 16;    /* TXD_W0_DATABYTE_COUNT */
    /* --- word 1 --- */
    w1 |= (2u << 6);    /* AIFS  */
    w1 |= (4u << 8);    /* CWMIN */
    w1 |= (10u << 12);  /* CWMAX */
    /* --- word 2 --- 1 Mbps CCK; PLCP length in usec (data_len*8) split hi/lo */
    data_len = (ULONG)frame_len + 4u;           /* HW appends the 4-byte FCS */
    plcp_duration = data_len * 8u;
    w2 = 0;
    w2 |= 0x04u << 8;                           /* PLCP_SERVICE */
    w2 |= (plcp_duration & 0xffu) << 16;        /* PLCP_LENGTH_LOW */
    w2 |= ((plcp_duration >> 8) & 0xffu) << 24; /* PLCP_LENGTH_HIGH */

    /* transfer length shaping (send_80211_frame:1016-1020): even + avoid exact
     * 512-byte multiple (add 2 zero pad bytes to break the ZLP boundary). */
    transfer_len = (int)(20 + frame_len);
    if (transfer_len & 1) transfer_len++;
    if ((transfer_len % 512) == 0) transfer_len += 2;

    /* Serialize the whole guardian+frame pair AND the shared TxScratch. TxLock is
     * a WDFWAITLOCK -> acquire is PASSIVE-only; every caller here is PASSIVE. */
    WdfWaitLockAcquire(ctx->TxLock, NULL);

    RtlZeroMemory(ctx->TxScratch, (SIZE_T)transfer_len);
    NwcPut32(ctx->TxScratch + 0, w0);
    NwcPut32(ctx->TxScratch + 4, w1);
    NwcPut32(ctx->TxScratch + 8, w2);
    /* words 3,4 (IV) stay zero for unencrypted TX */
    RtlCopyMemory(ctx->TxScratch + 20, frame, frame_len);

    /* Guardian: 1-byte OUT kick, then the descriptor+frame OUT. The synchronous
     * path needs the guardian for EVERY frame (send_80211_frame:1022-1051). */
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, &guardian, 1);
    st = WdfUsbTargetPipeWriteSynchronously(ctx->BulkOutPipe, NULL, NULL, &md, &sent);
    if (NT_SUCCESS(st)) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, ctx->TxScratch, (ULONG)transfer_len);
        st = WdfUsbTargetPipeWriteSynchronously(ctx->BulkOutPipe, NULL, NULL, &md, &sent);
    }

    InterlockedIncrement(&ctx->TxCount);
    ctx->LastTxStatus = st;
    if (!NT_SUCCESS(st)) InterlockedIncrement(&ctx->TxFail);

    WdfWaitLockRelease(ctx->TxLock);
    return st;
}

/* send_probe_response:1093 / send_auth_response:1170 / send_assoc_response:1186
 * thin wrappers: build into a small stack buffer (<=256), then NwcSend80211Frame. */
static VOID NwcSendProbeResponse(PDEVICE_CONTEXT ctx, const UCHAR dst[6],
                                 const CHAR *ssid, ULONG ssid_len)
{
    UCHAR frame[256];
    size_t n = NwcBuildProbeResponse(frame, sizeof(frame), ctx->Mac, dst, ssid, ssid_len,
                                     ctx->Channel, ctx->Privacy);
    if (n) NwcSend80211Frame(ctx, frame, n, TRUE /*timestamp*/, TRUE /*ack*/);
}

static VOID NwcSendAuthResponse(PDEVICE_CONTEXT ctx, const UCHAR dst[6],
                                USHORT alg, USHORT seq, USHORT status, BOOLEAN challenge)
{
    UCHAR frame[192];
    size_t n = NwcBuildAuthResponse(frame, sizeof(frame), ctx->Mac, dst, alg, seq, status, challenge);
    if (n) NwcSend80211Frame(ctx, frame, n, FALSE /*timestamp*/, TRUE /*ack*/);
}

static VOID NwcSendAssocResponse(PDEVICE_CONTEXT ctx, const UCHAR dst[6], USHORT aid, BOOLEAN reassoc)
{
    UCHAR frame[64];
    size_t n = NwcBuildAssocResponse(frame, sizeof(frame), ctx->Mac, dst, aid, reassoc, ctx->Privacy);
    if (n) NwcSend80211Frame(ctx, frame, n, FALSE /*timestamp*/, TRUE /*ack*/);
}

/* nwcusb_probe.c send_connector_response : 1115-1148.
 * Provided for parity (explicit 32-byte SSID with ssid[9] grant bit). The LIVE
 * grant path is handled inline in NwcMaybeAnswerManagement (rssid_buf), matching
 * the user-mode code which routes the registration reply through the probe-resp
 * builder. Kept here byte-faithful in case you want the dedicated form. */
static VOID NwcSendConnectorResponse(PDEVICE_CONTEXT ctx, const UCHAR dst[6],
                                     const CHAR *base_ssid, ULONG base_len, BOOLEAN accepted)
{
    UCHAR s[32];
    UCHAR frame[256];
    UCHAR *p = frame;
    ULONG n = base_len; if (n > 32) n = 32;
    RtlFillMemory(s, sizeof(s), 0x20);
    RtlCopyMemory(s, base_ssid, n);
    if (accepted) {
        s[9] = (UCHAR)(s[9] | 0x01);
    } else {
        s[9] = (UCHAR)(s[9] & 0x0e);
        RtlZeroMemory(s + 12, 20);
    }
    NwcPut16(p,0x0050); p+=2;
    NwcPut16(p,0x0000); p+=2;
    RtlCopyMemory(p,dst,6); p+=6;
    RtlCopyMemory(p,ctx->Mac,6); p+=6;
    RtlCopyMemory(p,ctx->Mac,6); p+=6;
    NwcPut16(p,0x0000); p+=2;
    RtlZeroMemory(p,8); p+=8;
    NwcPut16(p,100); p+=2;
    NwcPut16(p,0x0011); p+=2;                  /* ESS + Privacy */
    *p++=0; *p++=32; RtlCopyMemory(p,s,32); p+=32;
    *p++=1; *p++=4; *p++=0x82; *p++=0x84; *p++=0x0b; *p++=0x16;
    *p++=3; *p++=1; *p++=ctx->Channel;
    NwcSend80211Frame(ctx, frame, (size_t)(p - frame), TRUE, TRUE);
}

/* =========================================================================
 * RX CLASSIFIERS  (nwcusb_probe.c 1201-1314)
 * ========================================================================= */

/* is_probe_request:1201 */
static BOOLEAN NwcIsProbeRequest(const UCHAR *frame, int len)
{
    USHORT fc;
    if (len < 24) return FALSE;
    fc = NwcGet16(frame);
    return (BOOLEAN)(((fc >> 2) & 0x3) == 0 && ((fc >> 4) & 0xf) == 4);
}

/* is_mgmt_subtype:1209 */
static BOOLEAN NwcIsMgmtSubtype(const UCHAR *frame, int len, UCHAR subtype)
{
    USHORT fc;
    if (len < 24) return FALSE;
    fc = NwcGet16(frame);
    return (BOOLEAN)(((fc >> 2) & 0x3) == 0 && ((fc >> 4) & 0xf) == subtype);
}

/* mgmt_ie_offset:1237 */
static int NwcMgmtIeOffset(const UCHAR *frame, int len)
{
    USHORT fc; UCHAR type, subtype;
    if (len < 24) return -1;
    fc = NwcGet16(frame);
    type = (UCHAR)((fc >> 2) & 0x3);
    subtype = (UCHAR)((fc >> 4) & 0xf);
    if (type != 0) return -1;
    if (subtype == 4 || subtype == 0 || subtype == 2) return 24;
    if (subtype == 5 || subtype == 8) return 36;
    return -1;
}

/* should_answer_probe:1217 (NWC_ALLPROBE baked off -> NWCUSBAP-only) */
static BOOLEAN NwcShouldAnswerProbe(const UCHAR *frame, int len)
{
    int ie;
    if (!NwcIsProbeRequest(frame, len)) return FALSE;
    ie = NwcMgmtIeOffset(frame, len);
    if (ie >= 0 && ie + 2 + 8 <= len && frame[ie] == 0 && frame[ie + 1] >= 8 &&
        RtlEqualMemory(frame + ie + 2, "NWCUSBAP", 8))
        return TRUE;
    return FALSE;
}

/* is_for_ap:1287 */
static BOOLEAN NwcIsForAp(const UCHAR *frame, int len, const UCHAR mac[6])
{
    if (len < 24) return FALSE;
    return (BOOLEAN)(NwcMacEqual(frame + 4, mac) || NwcMacEqual(frame + 16, mac) || NwcMacBroadcast(frame + 4));
}

/* reg_is_accepted_now:1154 — two-phase grant table (thread-private, no lock). */
static BOOLEAN NwcRegIsAcceptedNow(PDEVICE_CONTEXT ctx, const UCHAR mac[6])
{
    ULONGLONG now = NwcNowMs();
    int i;
    for (i = 0; i < 16; i++)
        if (ctx->Reg[i].used && NwcMacEqual(ctx->Reg[i].mac, mac))
            return (BOOLEAN)((now - ctx->Reg[i].firstMs) >= NWC_REG_GRANT_MS);
    for (i = 0; i < 16; i++)
        if (!ctx->Reg[i].used) {
            RtlCopyMemory(ctx->Reg[i].mac, mac, 6);
            ctx->Reg[i].firstMs = now;
            ctx->Reg[i].used = 1;
            return FALSE;
        }
    return TRUE;
}

/* auth_answered_recently:1298 — de-dup table (thread-private). NWC_NODEDUP off. */
static BOOLEAN NwcAuthAnsweredRecently(PDEVICE_CONTEXT ctx, const UCHAR src[6], USHORT seq)
{
    ULONGLONG now = NwcNowMs();
    int i;
    for (i = 0; i < 16; i++)
        if (ctx->Auth[i].used && ctx->Auth[i].seq == seq && RtlEqualMemory(ctx->Auth[i].mac, src, 6)) {
            if (now - ctx->Auth[i].ms < NWC_AUTH_DEDUP_MS) return TRUE;
            ctx->Auth[i].ms = now; return FALSE;
        }
    for (i = 0; i < 16; i++)
        if (!ctx->Auth[i].used || now - ctx->Auth[i].ms > NWC_AUTH_SLOT_FREE_MS) {
            RtlCopyMemory(ctx->Auth[i].mac, src, 6);
            ctx->Auth[i].seq = seq; ctx->Auth[i].ms = now; ctx->Auth[i].used = 1;
            return FALSE;
        }
    return FALSE;
}

/* =========================================================================
 * THE RESPONDER  (nwcusb_probe.c maybe_answer_management : 1316-1443)
 * Called only from the PASSIVE responder thread.
 * ========================================================================= */
static VOID NwcMaybeAnswerManagement(PDEVICE_CONTEXT ctx, const UCHAR *frame, int len)
{
    const UCHAR *src;
    if (len < 24) return;
    src = frame + 10;

    /* --- probe / connector registration --- */
    if (NwcShouldAnswerProbe(frame, len)) {
        int ie = NwcMgmtIeOffset(frame, len);
        if (ie >= 0 && ie + 2 + 32 <= len && frame[ie] == 0 && frame[ie + 1] == 32 &&
            RtlEqualMemory(frame + ie + 2, "NWCUSBAP", 8)) {
            /* Two-phase grant: PENDING for ~2s (plain SSID), then GRANT by
             * setting ssid[9] |= 1 while KEEPING the 20 digits (rt25usbap.sys
             * @0x23d33 accepted form). (maybe_answer_management:1349-1359) */
            BOOLEAN grant = NwcRegIsAcceptedNow(ctx, src);
            CHAR rssid_buf[33];
            ULONG sl = ctx->SsidLen; if (sl > 32) sl = 32;
            RtlCopyMemory(rssid_buf, ctx->Ssid, sl); rssid_buf[sl] = '\0';
            if (grant && sl >= 10)
                rssid_buf[9] = (CHAR)((UCHAR)rssid_buf[9] | 0x01);
            NwcSendProbeResponse(ctx, src, rssid_buf, sl);
            return;
        }
        NwcSendProbeResponse(ctx, src, ctx->Ssid, ctx->SsidLen);
        return;
    }

    if (!NwcIsForAp(frame, len, ctx->Mac)) return;

    /* --- authentication (subtype 11) --- */
    if (NwcIsMgmtSubtype(frame, len, 11) && len >= 30) {
        USHORT fc = NwcGet16(frame);
        const UCHAR *auth = frame + 24;
        int auth_len = len - 24;
        USHORT alg, seq, status;

        if ((fc & 0x4000) != 0) {                 /* Protected */
            UCHAR wep_key[13];
            int plain_len = 0;
            BOOLEAN sw_ok = FALSE;
            if (ctx->Privacy && ctx->SsidLen == 32) {
                NwcDeriveOriginalWepKey((const UCHAR *)(ctx->Ssid + 12), wep_key);
                if (NwcWepDecryptBody(frame + 24, len - 24, wep_key,
                                      ctx->WepDecrypt, ctx->AuthPlain, sizeof(ctx->AuthPlain), &plain_len)) {
                    auth = ctx->AuthPlain; auth_len = plain_len; sw_ok = TRUE;
                }
            }
            if (!sw_ok) {
                /* HW WEP-decrypt in place: IV stays at frame+24, plaintext auth
                 * body at frame+28 (maybe_answer_management:1390-1400). */
                auth = frame + 28; auth_len = len - 28;
            }
        }
        if (auth_len < 6) return;
        alg    = NwcGet16(auth);
        seq    = NwcGet16(auth + 2);
        status = NwcGet16(auth + 4);
        (void)status;
        if (NwcAuthAnsweredRecently(ctx, src, seq))   /* stay off-air for the SIFS auto-ACK */
            return;
        if (alg == 0 && seq == 1)      NwcSendAuthResponse(ctx, src, alg, 2, 0, FALSE);
        else if (alg == 1 && seq == 1) NwcSendAuthResponse(ctx, src, alg, 2, 0, TRUE);
        else if (alg == 1 && seq == 3) NwcSendAuthResponse(ctx, src, alg, 4, 0, FALSE);
        return;
    }

    /* --- (re)assoc request (subtype 0 / 2) --- */
    if (NwcIsMgmtSubtype(frame, len, 0) || NwcIsMgmtSubtype(frame, len, 2)) {
        BOOLEAN reassoc = NwcIsMgmtSubtype(frame, len, 2);
        NwcSendAssocResponse(ctx, src, 1, reassoc);
        return;
    }
}

/* =========================================================================
 * RX FRAMING / DISPATCH  (nwcusb_probe.c log_rx_packet : 1573-1657, core only)
 * The continuous reader delivers the 802.11 frame at offset 0 with the 16-byte
 * RXD as a trailer (+ 4-byte FCS). A legacy alt framing prepends a 16-byte
 * descriptor; both branches preserved. Logging paths dropped.
 * ========================================================================= */
static VOID NwcProcessRxFrame(PDEVICE_CONTEXT ctx, const UCHAR *buf, int len)
{
    USHORT raw_fc;
    ULONG w0;
    USHORT data_len;
    const UCHAR *frame;
    int frame_avail;

    if (len < 2) return;

    /* Primary: buffer IS the 802.11 frame (RXD is the trailing 16 bytes). The
     * management parser only reads header+IEs, so the FCS+RXD trailer is inert.
     * (log_rx_packet:1619-1625) */
    raw_fc = NwcGet16(buf);
    if ((raw_fc & 0x0003) == 0 && ((raw_fc >> 2) & 0x3) <= 2) {
        NwcMaybeAnswerManagement(ctx, buf, len);
        return;
    }

    /* Alt: 16-byte RT2570 RX descriptor PREFIX, 802.11 frame at +16.
     * (log_rx_packet:1627-1651) */
    if (len < 16) return;
    w0 = NwcGet32(buf + 0);
    data_len = (USHORT)((w0 >> 16) & 0x0fff);
    frame = buf + 16;
    frame_avail = len - 16;
    if (data_len < frame_avail) frame_avail = data_len;
    if (frame_avail >= 24)
        NwcMaybeAnswerManagement(ctx, frame, frame_avail);
}

/* =========================================================================
 * HARDWARE BEACON RING LOAD  (port of rt2500usb_write_beacon + rt25usbap.sys 0x21220)
 * -------------------------------------------------------------------------
 * Load the beacon template into the RT2570's hardware beacon ring and enable the
 * autonomous BEACON_GEN engine, so the hardware TSF/beacon block genuinely runs.
 * That live TSF/beacon block is what arms the SIFS auto-responder (the DS's auth
 * seq1 hardware-ACK). User mode could never make this radiate because every step
 * was a high-latency USB round-trip; in-kernel we own the pipe with proper timing.
 * Done ONCE at go-live; the chip then re-emits the beacon every TBTT on its own.
 * ========================================================================= */
#define NWC_TXRX_CSR19 0x0466
static NTSTATUS NwcLoadHwBeacon(PDEVICE_CONTEXT ctx)
{
    UCHAR bframe[256];
    size_t bn;
    /* 1. Disable BEACON_GEN before reloading the ring (never let the generator read
     *    half-written beacon data). */
    NwcRegWrite(ctx, NWC_TXRX_CSR19, 0x0000);
    /* 2. Build the beacon and upload it into the beacon ring: guardian byte + the
     *    TXD+frame bulk-OUT (NwcSend80211Frame does exactly that pair). TIMESTAMP=1 so
     *    the hardware stamps the live TSF into the beacon; no ACK (broadcast). */
    bn = NwcBuildBeacon(bframe, sizeof(bframe), ctx->Mac, ctx->Ssid, ctx->SsidLen,
                        ctx->Channel, ctx->Privacy);
    if (!bn) return STATUS_UNSUCCESSFUL;
    NwcSend80211Frame(ctx, bframe, bn, TRUE /*timestamp*/, FALSE /*no ack*/);
    /* 3. Enable BEACON_GEN with the alternating enable/disable dance
     *    (0x1d -> 0 -> 0x1d -> 0 -> 0x1d); 0x1d = TSF_COUNT|TSF_SYNC=2|TBCN|BEACON_GEN.
     *    rt2500usb: "Beacon generation will fail initially" without this dance. */
    NwcRegWrite(ctx, NWC_TXRX_CSR19, 0x001d);
    NwcRegWrite(ctx, NWC_TXRX_CSR19, 0x0000);
    NwcRegWrite(ctx, NWC_TXRX_CSR19, 0x001d);
    NwcRegWrite(ctx, NWC_TXRX_CSR19, 0x0000);
    NwcRegWrite(ctx, NWC_TXRX_CSR19, 0x001d);
    return STATUS_SUCCESS;
}

/* =========================================================================
 * RESPONDER SYSTEM THREAD  (PASSIVE_LEVEL) + start/stop
 * ========================================================================= */
static KSTART_ROUTINE NwcResponderThread;
static VOID NwcResponderThread(PVOID Context)
{
    /* SINGLE-THREAD design (lower risk than a separate passive WDF timer): this one
     * PASSIVE thread does BOTH the RX responder AND the ~100ms beacon, via a wait
     * timeout. Because it is the sole TX caller, there is no TX concurrency and TxLock
     * is uncontended. WdfUsbTargetPipeWriteSynchronously is legal here (PASSIVE). */
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    PVOID waitObjs[2];
    NTSTATUS wst;
    LARGE_INTEGER beaconTo;
    ULONGLONG lastBeacon = NwcNowMs();
    UCHAR bframe[256];
    size_t bn;
    BOOLEAN hwBeaconDone = FALSE;   /* HW beacon-ring load happens once at go-live */
    BOOLEAN hwBeaconOk   = FALSE;   /* if HW beacon loaded, suppress SW beacon (single TX engine) */

    waitObjs[0] = &ctx->RxEvent;
    waitObjs[1] = &ctx->StopEvent;
    beaconTo.QuadPart = -(LONGLONG)NWC_BEACON_PERIOD_MS * 10000LL;  /* 100ms relative */

    for (;;) {
        wst = KeWaitForMultipleObjects(2, waitObjs, WaitAny, Executive,
                                       KernelMode, FALSE, &beaconTo, NULL);
        if (wst == STATUS_WAIT_1 || InterlockedCompareExchange(&ctx->Stopping, 0, 0) != 0)
            break;   /* StopEvent (or stop flag) -> exit */

        /* On first config: load the HARDWARE beacon into the ring + enable BEACON_GEN
         * ONCE (the chip then auto-beacons every TBTT -> live TSF -> armed responder). */
        if (InterlockedCompareExchange(&ctx->Configured, 0, 0) != 0 && !hwBeaconDone) {
            hwBeaconOk = NT_SUCCESS(NwcLoadHwBeacon(ctx));
            hwBeaconDone = TRUE;
        }
        /* Software beacon runs ONLY if the HW beacon did not load -- the RT2570 has a
         * SINGLE TX engine, so a SW beacon colliding with the HW BEACON_GEN wrecks both
         * (DS then finds neither). With the HW beacon up, rely on it alone. */
        if (InterlockedCompareExchange(&ctx->Configured, 0, 0) != 0 && !hwBeaconOk &&
            (NwcNowMs() - lastBeacon) >= NWC_BEACON_PERIOD_MS) {
            bn = NwcBuildBeacon(bframe, sizeof(bframe), ctx->Mac, ctx->Ssid, ctx->SsidLen,
                                ctx->Channel, ctx->Privacy);
            if (bn) NwcSend80211Frame(ctx, bframe, bn, TRUE /*timestamp*/, FALSE /*no ack*/);
            lastBeacon = NwcNowMs();
        }
        if (wst == STATUS_TIMEOUT)
            continue;

        /* RxEvent: drain the entire ring. Copy each frame out UNDER RxLock, release,
         * then parse+respond OUTSIDE the lock (never hold the spinlock across TX). */
        for (;;) {
            ULONG n = 0;
            WdfSpinLockAcquire(ctx->RxLock);
            if (ctx->RxTail != ctx->RxHead) {
                n = ctx->Rx[ctx->RxTail].len;
                if (n > RX_FRAME_MAX) n = RX_FRAME_MAX;
                RtlCopyMemory(ctx->RxWork, ctx->Rx[ctx->RxTail].buf, n);
                ctx->RxTail = (ctx->RxTail + 1) % RX_RING_DEPTH;
            }
            WdfSpinLockRelease(ctx->RxLock);

            if (n == 0) break;                 /* ring empty */
            if (InterlockedCompareExchange(&ctx->Configured, 0, 0) == 0)
                continue;                      /* discard until SET_APCONFIG arrives */
            NwcProcessRxFrame(ctx, ctx->RxWork, (int)n);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Start: call from NwcEvtPrepareHardware AFTER pipes + reader are configured. */
static NTSTATUS NwcResponderStart(PDEVICE_CONTEXT ctx)
{
    HANDLE hThread = NULL;
    NTSTATUS st;
    OBJECT_ATTRIBUTES oa;

    if (ctx->ThreadObj != NULL) return STATUS_SUCCESS;   /* already running */

    KeInitializeEvent(&ctx->RxEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
    InterlockedExchange(&ctx->Stopping, 0);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                              NwcResponderThread, ctx);
    if (!NT_SUCCESS(st)) return st;

    /* Reference the thread so we can join it in NwcResponderStop. ObjectType NULL
     * = "any type" (accepted); the stricter form is *PsThreadType (needs ntifs.h). */
    st = ObReferenceObjectByHandle(hThread, SYNCHRONIZE, NULL, KernelMode,
                                   &ctx->ThreadObj, NULL);
    ZwClose(hThread);
    if (!NT_SUCCESS(st)) {
        /* Could not get the object: signal the thread to exit so it doesn't leak. */
        InterlockedExchange(&ctx->Stopping, 1);
        KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        ctx->ThreadObj = NULL;
        return st;
    }
    return STATUS_SUCCESS;
}

/* Stop: call from EvtDeviceReleaseHardware (or D0Exit). PASSIVE. Joins cleanly. */
static VOID NwcResponderStop(PDEVICE_CONTEXT ctx)
{
    if (ctx->ThreadObj == NULL) return;

    InterlockedExchange(&ctx->Stopping, 1);
    KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(ctx->ThreadObj, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(ctx->ThreadObj);
    ctx->ThreadObj = NULL;
}

/* BEACON: folded into NwcResponderThread above (single PASSIVE thread does RX
 * responder + ~100ms beacon via the wait timeout). No separate WDF timer, so no
 * passive-timer IRQL risk and no TX concurrency. (nwcusb_probe.c send_beacon:1081-1090) */

/* =========================================================================
 * IOCTL_NWC_SET_APCONFIG handler body (add a case to NwcEvtIoDeviceControl)
 * User mode hands down {mac, ssid, ssid_len, channel, privacy} AFTER its proven
 * radio init, flips Configured, and starts the beacon. The user-mode MAC is now
 * POLICY-ONLY (it no longer polls RX or sends responses).
 * ========================================================================= */
#if 0  /* ---- reference: paste as a case into the IOCTL switch ---- */
    case IOCTL_NWC_SET_APCONFIG: {
        NWC_APCONFIG *p; size_t inlen; ULONG sl;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*p), (PVOID*)&p, &inlen);
        if (!NT_SUCCESS(status)) break;
        RtlCopyMemory(ctx->Mac, p->mac, 6);
        sl = p->ssid_len; if (sl > 32) sl = 32;
        RtlZeroMemory(ctx->Ssid, sizeof(ctx->Ssid));
        RtlCopyMemory(ctx->Ssid, p->ssid, sl);
        ctx->SsidLen = sl;
        ctx->Channel = p->channel ? p->channel : 1;
        ctx->Privacy = p->privacy ? TRUE : FALSE;
        RtlZeroMemory(ctx->Reg,  sizeof(ctx->Reg));
        RtlZeroMemory(ctx->Auth, sizeof(ctx->Auth));
        InterlockedExchange(&ctx->Configured, 1);   /* responder thread begins beaconing */
        status = STATUS_SUCCESS;
        break;
    }
#endif

/* =========================================================================
 * NwcEvtBulkInComplete HOOK — one added line after the ring enqueue.
 * Replace the current completion routine's body tail so it signals the thread.
 * (Only the KeSetEvent line is new; the ring copy is unchanged from nwcusbap.c.)
 * ========================================================================= */
#if 0  /* ---- reference: the modified completion routine ---- */
VOID NwcEvtBulkInComplete(WDFUSBPIPE Pipe, WDFMEMORY Buffer, size_t NumBytes, WDFCONTEXT Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    PUCHAR data; LONG next;
    UNREFERENCED_PARAMETER(Pipe);
    InterlockedIncrement(&ctx->RxComplete);
    if (NumBytes == 0 || NumBytes > RX_FRAME_MAX) return;
    InterlockedExchangeAdd(&ctx->RxBytes, (LONG)NumBytes);
    data = (PUCHAR)WdfMemoryGetBuffer(Buffer, NULL);
    WdfSpinLockAcquire(ctx->RxLock);
    next = (ctx->RxHead + 1) % RX_RING_DEPTH;
    if (next != ctx->RxTail) {
        ctx->Rx[ctx->RxHead].len = (ULONG)NumBytes;
        RtlCopyMemory(ctx->Rx[ctx->RxHead].buf, data, NumBytes);
        ctx->RxHead = next;
    } else {
        InterlockedIncrement(&ctx->RxDropped);
    }
    WdfSpinLockRelease(ctx->RxLock);
    KeSetEvent(&ctx->RxEvent, IO_NO_INCREMENT, FALSE);   /* <-- NEW: wake responder (DISPATCH-safe) */
}
#endif
