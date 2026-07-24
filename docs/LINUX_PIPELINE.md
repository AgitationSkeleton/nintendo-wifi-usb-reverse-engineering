# NWC Connector — Linux NUC + AR9271 Sniffer Session (2026-07-22)

**Headline:** Using a second dongle (AR9271) as an over-the-air monitor of our own connector AP,
we could finally *watch* the DS↔dongle auth handshake directly. This **refuted the long-standing
"SIFS auto-ACK wall"** and re-localized the real blocker. It also turned up a concrete, source-
verified path forward: **hardware beaconing done the way the original driver does it.**

The DS still ends at error **51303**, but the failure is now precisely understood, not mysterious.

---

## Rig

- **RT2570** (Nintendo dongle, `VID_0411 PID_008B`, BSSID `AA:BB:CC:DD:EE:FF`) runs our exact
  Win10 connector code, `nwcusb_probe`, compiled on Linux against libusb (non-KMDF backend).
  `ap-loop 1` = connector SoftAP on channel 1.
- **AR9271** (`0cf3:9271`, iface `wlx112233445566`) in **monitor mode on ch1** = over-the-air
  sniffer of *our own* network. Control test passed (it captures ambient ACK frames), so a
  negative result (no frame on air) is real, not a deaf receiver.
- NUC `user@192.0.2.10`, driven entirely over SSH from the main PC. Both dongles on USB bus 2.

## Breakthrough 1 — the hardware auto-ACK FIRES (old wall refuted)

Over-the-air capture of a real DS connection attempt (`captures/sniff_ds.pcap`):

```
[connector] registration probe from 12:34:56:78:9a:bc name="PlayerName" -> GRANT   (x246)
auth  TA=DS  RA=ours   seq=1                 <- DS sends WEP auth seq1
ACK          RA=DS                            <- the RT2570 ACKs it (every single seq1)
```

Every `seq1` the DS sends gets a hardware ACK addressed back to it. **The auto-ACK is not the
problem.** Months of work premised on "the RT2570 can't auto-ACK the DS" were chasing a ghost.

## Breakthrough 2 — our seq2 auth-response NEVER radiates (the real wall)

Same capture: the RT2570 radiates **1402 beacons, ~291 probe-responses, and ZERO auth frames**,
even though our software builds seq2 and the USB write *succeeds* every time
(`[tx] auth-response sent`, 0 failures). The DS never receives seq2 → never advances to seq3 →
51303.

### Exhaustive bisection (self-test TX to dummy dests + 5 real DS runs)

| Frame | Destination | Radiates? |
|---|---|---|
| Beacon (ack=0) | broadcast | YES |
| Probe-response (ack=1, ts=1) | the DS | YES |
| Auth-response (any size) | a **dummy** MAC | YES |
| Auth-response — ts 0/1, ack 0/1, 30B/160B, throttled | the **real DS** | **NO, every time** |

It is **not** the descriptor flags, frame size, or the probe-request flood. The only frames that
never radiate are **auth-subtype frames addressed to the station currently mid-authentication.**

## AP-mode exploration — and why it fails for us

`TXRX_CSR19` final value: ours `0x1d` vs the working `rt2500usb` `0x1f` — the **TSF_SYNC** field:
ours = 2 (ADHOC), rt2500usb = 3 (AP/master). Testing TSF_SYNC=3:

- It **breaks discovery** — AP-sync destabilizes our *software* beacon so badly the DS can't see
  NWCUSBAP at all.
- adhoc-sync (TSF_SYNC=2) is required for discovery, but that's the mode where seq2 won't radiate.

They are mutually exclusive **unless we implement real hardware beaconing.** That is the crux.

## Register diff vs rt2500usb (same dongle, usbmon)

Decoded via `tshark 'usb.bmRequestType==0x40 && usb.setup.bRequest==6'` (wIndex=offset,
`usb.data_fragment`=value LE). **Our probe issues 123 vendor writes; rt2500usb issues 617.**

- **`TXRX_CSR18` bug fixed:** ours computed `0x1900` (INTERVAL×4, from a misread of rt2500usb);
  correct is `0x0640` (INTERVAL=100 TU). Added `NWC_CSR18`.
- `TXRX_CSR19` TSF_SYNC 2 vs 3 (above).
- rt2500usb does 6-byte block writes of MAC→`0x0404` and BSSID→`0x040a` that we skip.

## Hardware beaconing — the corrected path (source-verified)

With software beacon **off**, our HW beacon engine radiates **nothing** (STA_CSR5 frozen). Our
`send_beacon` bulk-transmits a one-shot frame; it never sets up the HW beacon the chip repeats.

My first attempt (`hw_load_beacon`, `NWC_HWBEACON`) wrote the beacon to register `0x2c00` — this
**was the wrong mechanism** (load reports OK but nothing fires). The actual `rt2500usb.c`
`rt2500usb_write_beacon` (see `rt2500usb-src/`, lines ~1123-1176, callback ~1295-1307) shows:

1. `pipe = usb_sndbulkpipe(usb_dev, entry->queue->usb_endpoint)` — the **BEACON queue's own
   endpoint**, *distinct from the data endpoint* our probe uses for everything.
2. Disable `TXRX_CSR19_BEACON_GEN`.
3. Prepend `TXD_DESC_SIZE` and write the TX descriptor.
4. Fill (not yet submit) the beacon URB; fill a **1-byte guardian URB** to the same pipe.
5. **Submit the guardian first**; in `beacondone`, when the guardian completes, **submit the
   beacon frame**.
6. Toggle `TXRX_CSR19`: `TSF_COUNT|TBCN` on, then BEACON_GEN **on/off/on/off/on** (ends ON).

**So the fix is: send the beacon (guardian-first, then frame) to the BEACON endpoint — not the
data endpoint — with the CSR19 toggle. No `0x2c00` write.**

## Next steps (in priority order)

1. Enumerate the RT2570's bulk-OUT endpoints; identify the **beacon endpoint** (`queue->usb_endpoint`).
   rt2x00usb assigns queue endpoints from the USB descriptors — replicate that mapping.
2. Reimplement `hw_load_beacon` to bulk-send guardian→beacon to the **beacon endpoint**, then the
   CSR19 toggle. Verify `STA_CSR5` counts / HW beacons appear on air with SW beacon off.
3. With a live HW beacon, enable AP mode (TSF_SYNC=3). Discovery should now hold.
4. Re-test the DS: in true AP mode the auto-responder should carry seq2/auth the way the original
   driver does on **real XP hardware** (which is reliably successful — the old "1 pass" limit was
   a VM USB-passthrough artifact, not an inherent limit).

## Files in this drop

- `probe/nwcusb_probe.c`, `probe/linux_compat.h`, `probe/build.sh`, `probe/README.md` — the
  Linux-buildable connector probe + this session's new knobs.
- `rt2500usb-src/` — the upstream kernel `rt2500usb` + `rt2x00` reference source (the ground truth
  for the beacon mechanism) + `NOTES.md`.
- `analysis/` — the reusable usbmon register-decoder and over-the-air sniff/analyze tools.
- `captures/` — key over-the-air + usbmon evidence pcaps.

### New env knobs added this session
`NWC_AUTH_TS`, `NWC_AUTH_NOACK`, `NWC_AUTH_NOCHAL`, `NWC_PROBE_MINGAP_MS`, `NWC_TXTEST`,
`NWC_CSR18`, `NWC_HWBEACON`.

---

## ADDENDUM 2026-07-24 — HW beacon reimplemented, and a reframe

**RT2570 has only 2 endpoints** (`0x81 IN`, `0x01 OUT`, both bulk) — no separate beacon endpoint,
and the 20-byte TXD is generic (no "beacon" bit). Rewrote `hw_load_beacon` to the real
`rt2500usb_write_beacon` sequence (bulk on EP `0x01`, **not** a `0x2c00` register write): disable
BEACON_GEN → 1-byte guardian → CSR19 `on/off/on/off/on` toggle → `[20B TXdesc][beacon]` frame.
Added a `raw_bulk_out` helper; `NWC_HWBEACON` now keeps the software beacon on.

**Result:** `STA_CSR5` (the "Beacon sent counter") now **increments** (15/16/99) — the hardware
beacon *engine* runs, where before it was frozen at 0. But the beacon *content* still never
radiates with our BSSID (likely the synchronous-libusb ~5ms gap between the guardian and the
beacon frame vs rt2500usb's tight async submission, so the frame misses the beacon-load window).

**The reframe (important):** `rt2500usb.h` documents `TXRX_CSR19_TSF_SYNC` as **`2 = ad-hoc/master
mode`** — the value we already use (`CSR19=0x1d`). So we were in master/beaconing mode all along;
`STA_CSR5=0` only ever meant "no *hardware* beacons" (we software-beacon), **not** a dead TSF
timer. `TSF_SYNC=3` (which I had chased as "AP mode") is non-standard and is what broke discovery.
**So "dead TSF → need HW beaconing" was probably a misdiagnosis** — the seq2 wall is not about the
beacon/TSF mode.

**New leading hypothesis for the seq2 wall:** the hardware **auto-responder** (same engine as the
auto-ACK) claims the auth-response slot for the station that just sent `seq1` (and got auto-ACKed)
and **blocks our software `seq2`**, without emitting a valid `seq2` itself. This fits every
observation: auth→mid-auth-DS never radiates, while probe-resp→DS (no auto-ACK) and auth→dummy
(no `seq1`/auto-ACK) both radiate. **Next test:** disable the auto-responder
(`TXRX_CSR3`/`CSR4`/`CSR10`) and check via the AR9271 whether `seq2` then radiates to the DS
(auto-ACK will break — that's the diagnostic). If yes, find an auto-responder config that ACKs
but lets the software auth TX through (or makes the hardware emit the correct `seq2`).

**Remote access to the NUC:** `xrdp`+XFCE on `192.0.2.10:3389` → `mstsc /v:192.0.2.10` (login
= the NUC user's account password). Terminal helper: SSH (see repo README) — the remote-pilot helpers are omitted from the public repo.
(`.\nuc-remote.ps1`, `-Command "..."`, `-Desktop`).

---

## ★★★ SOLVED (802.11 layer) 2026-07-24 — it was the GUARDIAN byte

We had `send_80211_frame` prepend a 1-byte USB "guardian" to **every** frame. The usbmon capture
of the **working original XP driver** (`captures/xp_dongle_DS_full_interaction.pcapng`) shows the
guardian precedes **only the beacon** — data/mgmt frames, including the seq2 auth-response
(frame 15447: a bare 180-byte bulk-OUT), have **no guardian**. The guardian before our unicast
seq2 was derailing that TX.

**Fix:** `send_80211_frame` now sends the guardian only when the frame label contains "beacon"
(`NWC_GUARD_ALL` restores the old behaviour).

**Result:** the DS **completes WEP auth, associates, shows a GREEN signal**, and now fails with
**52003** (was 51303) — i.e. it is *past the entire 802.11/connector layer* and failing at the
**internet uplink / connection test**. (The AR9271 sniffer is lossy on the shared USB bus and
missed the mgmt frames that run; the DS's own error-code jump + data frames are ground truth.)

### How the XP capture was read (gold reference)
- Linux usbmon pcapng, 28430 pkts. Original driver register writes = `USB_SINGLE_WRITE`
  (bRequest=2, value in `wValue`, offset in `wIndex`); reads = `MULTI_READ` (7).
- Full auth: seq1(RX ep0x81) → seq2(TX ep0x01, 180B) → seq3(RX, WEP-encrypted) → seq4(TX, 50B).
- Register diff vs ours flagged `TXRX_CSR0` (security, KEY_ID 0xf vs 0), tested via `NWC_CSR0` —
  but the GUARDIAN was the actual fix.

## NEXT PHASE — internet uplink (DHCP + NAT + Wiimmfi)
Our probe is **management-only** (it TX'd 0 data frames). The DS associates but its DHCP /
connection-test traffic gets no reply → 52003. Needs a data path: RX + WEP-decrypt the DS's data
frames → a TAP interface → `dnsmasq` (DHCP + Wiimmfi DNS) + `iptables MASQUERADE` on `eno1` →
WEP-encrypt replies → TX back. (On XP this was ICS; see memory `nwc-xpvm-ds-online`.)

---

## ★★★★ FULLY ONLINE 2026-07-24 — DS on Wiimmfi, end to end

"Connection successful." The DS reached Wiimmfi through the NUC + RT2570 + our userspace probe:
connector grant -> WEP auth -> association -> **DHCP** (192.168.44.20, "NintendoDS") -> **DNS**
(conntest.wiimmfi.de via 164.132.44.106) -> **NAT** -> conntest **HTTP 200 OK** -> **POST /ac**
to the Wiimmfi NAS auth endpoint -> **200 OK** (login accepted).

### The final two fixes (data path)
1. **RX decode:** the RT2570 HW-decrypts WEP in place — the plaintext SNAP+payload sits at
   `frame + header + 4` (past the IV/keyid), with a trailing ICV. Software-decrypting it again
   gave ICV mismatches; `tap_rx_dsdata` now reads the HW plaintext directly (software decrypt is
   a fallback).
2. **TX frame cap:** `send_80211_frame` rejected frames > 512 bytes, silently dropping the
   conntest's ~536B HTTP response (52203 at the very last step). Raised to 1600.

### Data bridge (NWC_DATAPATH=1)
A TAP interface (`nwc0`) + software WEP: DS data frame -> HW-decrypted -> Ethernet -> TAP ->
dnsmasq(DHCP) + iptables MASQUERADE(eno1); replies TAP -> 802.11 + WEP-encrypt -> DS. dnsmasq
hands out Wiimmfi DNS `164.132.44.106`. One-shot bring-up: `nuc-setup/start-datapath.sh`.

**The project is done: a Nintendo DS goes fully online on Wiimmfi through a Linux box driving the
Nintendo Wi-Fi USB Connector dongle entirely from reverse-engineered userspace code.**
