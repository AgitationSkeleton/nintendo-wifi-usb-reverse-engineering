/* nwc_kmtest.c — VM SAFETY harness for the in-kernel MAC responder (nwcusbap.sys).
 *
 * No libusb, no dongle traffic needed. Opens \\.\NWCUSBAP, hands down an AP config
 * (starts the responder thread's beacon), then INJECTS crafted 802.11 frames into the
 * RX ring so the kernel responder exercises EVERY path — probe/registration-grant,
 * WEP shared-key auth seq1->seq2 + seq3->seq4 (incl. the WEP-decrypt + frame+28
 * fallback), and assoc — and polls STATS to confirm the driver stays alive (no bugcheck).
 *
 * Build (x86, matches the 32-bit test VM):
 *   cl /nologo /W3 nwc_kmtest.c /link
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "nwcusbap_ioctl.h"

static HANDLE g_dev;

static void ioctl_send(DWORD code, void *in, DWORD inlen, void *out, DWORD outlen, const char *tag)
{
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(g_dev, code, in, inlen, out, outlen, &ret, NULL);
    printf("  %-14s -> %s (gle=%lu ret=%lu)\n", tag, ok ? "OK" : "FAIL", ok ? 0 : GetLastError(), ret);
}

static void print_stats(const char *tag)
{
    NWC_STATS s; DWORD ret = 0;
    if (DeviceIoControl(g_dev, IOCTL_NWC_STATS, NULL, 0, &s, sizeof(s), &ret, NULL) && ret >= sizeof(s))
        printf("[stats %s] stage=%u select=0x%08lx reader=0x%08lx responder=0x%08lx pipes=0x%x | rx=%u tx=%u tx_fail=%u rx_drop=%u last_tx=0x%08lx\n",
               tag, s.start_stage, (unsigned long)s.select_status, (unsigned long)s.reader_config_status,
               (unsigned long)s.responder_status, s.pipe_flags,
               s.rx_complete, s.tx_count, s.tx_fail, s.rx_dropped, (unsigned long)s.last_tx_status);
    else {
        printf("[stats %s] FAILED gle=%lu ret=%lu\n", tag, GetLastError(), ret);
        return;
    }
    if (s.num_configs || s.config_desc_len) {
        unsigned int n = s.config_desc_len < 48 ? s.config_desc_len : 48, i;
        printf("    numCfg=%u cfgDescStatus=0x%08lx cfgDescLen=%u raw:", s.num_configs,
               (unsigned long)s.config_desc_status, s.config_desc_len);
        for (i = 0; i < n; i++) printf(" %02x", s.config_desc[i]);
        printf("\n");
    }
}

static const unsigned char OUR[6] = { 0x02,0x00,0x00,0x00,0x00,0x01 };  /* placeholder AP MAC; the real BSSID is read from the dongle EEPROM at runtime */
static const unsigned char DS[6]  = { 0x02,0x00,0x00,0x00,0x00,0x02 };  /* placeholder client MAC for the offline harness */
static const char SSID[32] = { 'N','W','C','U','S','B','A','P',' ',' ',' ',' ',
                               '1','2','3','4','5','1','2','3','4','5','1','2','3','4','5','1','2','3','4','5' };

/* build a raw 802.11 mgmt frame header (fc, dur, a1,a2,a3, seq). returns bytes written. */
static int hdr(unsigned char *f, unsigned char fc0, unsigned char fc1,
               const unsigned char *a1, const unsigned char *a2, const unsigned char *a3)
{
    f[0]=fc0; f[1]=fc1; f[2]=0; f[3]=0;
    memcpy(f+4,a1,6); memcpy(f+10,a2,6); memcpy(f+16,a3,6);
    f[22]=0; f[23]=0;
    return 24;
}

int main(void)
{
    unsigned char f[256];
    int n, i;
    NWC_APCONFIG apc;
    unsigned char bc[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

    g_dev = CreateFileA("<path>", GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_dev == INVALID_HANDLE_VALUE) { printf("open <path> failed gle=%lu (driver installed+started?)\n", GetLastError()); return 1; }
    printf("opened <path>");
    print_stats("init");

    /* 1) SET_APCONFIG -> responder thread starts beaconing */
    memset(&apc,0,sizeof(apc));
    memcpy(apc.mac, OUR, 6);
    memcpy(apc.ssid, SSID, 32); apc.ssid_len = 32;
    apc.channel = 1; apc.privacy = 1;
    ioctl_send(IOCTL_NWC_SET_APCONFIG, &apc, sizeof(apc), NULL, 0, "SET_APCONFIG");
    Sleep(400); print_stats("after-apconfig");   /* beacon should have TX'd */

    /* 2) probe-req w/ 32-byte NWCUSBAP SSID IE -> registration grant path */
    n = hdr(f, 0x40, 0x00, bc, DS, bc);
    f[n++]=0; f[n++]=32; memcpy(f+n,"NWCUSBAP    ",12); n+=12;
    f[n++]=0;f[n++]=1;f[n++]=0;f[n++]=0;                 /* ctrl */
    memcpy(f+n,"\x4d\x00\x61\x00\x78\x00",6); n+=6;      /* "Max" utf16 */
    while (n < 24+2+32) f[n++]=0;                        /* pad SSID IE to 32 */
    ioctl_send(IOCTL_NWC_INJECT_RX, f, n, NULL, 0, "inject probe");

    /* 3) auth seq1 (open+shared) -> seq2 (challenge) */
    n = hdr(f, 0xb0, 0x00, OUR, DS, OUR);
    f[n++]=1;f[n++]=0; f[n++]=1;f[n++]=0; f[n++]=0;f[n++]=0;   /* alg1 seq1 status0 */
    ioctl_send(IOCTL_NWC_INJECT_RX, f, n, NULL, 0, "inject auth1");

    /* 4) auth seq3 PROTECTED -> WEP-decrypt attempt + frame+28 fallback -> seq4 */
    n = hdr(f, 0xb0, 0x40, OUR, DS, OUR);   /* Protected bit set */
    f[n++]=0x11;f[n++]=0x22;f[n++]=0x33;f[n++]=0x00;          /* IV @ frame+24 */
    f[n++]=1;f[n++]=0; f[n++]=3;f[n++]=0; f[n++]=0;f[n++]=0;  /* alg1 seq3 status0 @ frame+28 */
    for (i=0;i<20;i++) f[n++]=(unsigned char)i;               /* filler so body_len>=8 */
    ioctl_send(IOCTL_NWC_INJECT_RX, f, n, NULL, 0, "inject auth3");

    /* 5) assoc-req -> assoc-resp */
    n = hdr(f, 0x00, 0x00, OUR, DS, OUR);
    f[n++]=0x11;f[n++]=0x00; f[n++]=0x01;f[n++]=0x00;          /* cap, listen */
    f[n++]=0;f[n++]=4; memcpy(f+n,"NWCU",4); n+=4;
    ioctl_send(IOCTL_NWC_INJECT_RX, f, n, NULL, 0, "inject assoc");

    /* 6) hammer: re-inject the auth burst to stress dedup + WEP repeatedly */
    for (i=0;i<50;i++) {
        n = hdr(f, 0xb0, (i&1)?0x08:0x00, OUR, DS, OUR);      /* alternate Retry bit */
        f[n++]=1;f[n++]=0; f[n++]=1;f[n++]=0; f[n++]=0;f[n++]=0;
        ioctl_send(IOCTL_NWC_INJECT_RX, f, n, NULL, 0, i==0?"inject burst":"");
        Sleep(5);
    }

    for (i=0;i<5;i++) { Sleep(1000); print_stats("alive"); }
    printf("DONE — if you see this and the VM did not bugcheck, the responder paths are crash-safe.\n");
    CloseHandle(g_dev);
    return 0;
}
