# Usage

> Read [BUILDING.md](BUILDING.md) first. Do driver work in a throwaway VM.

---

## 1. Install the driver and bind the dongle

```powershell
pnputil /add-driver nwcusbap.inf /install
```

Then confirm the dongle bound to it:

```powershell
Get-PnpDevice | ? InstanceId -match 'VID_0411&PID_008B' |
    Format-List FriendlyName,Status,Service   # expect Status=OK, Service=nwcusbap
```

If it doesn't bind, re-enumerate it (`Disable-PnpDevice` / `Enable-PnpDevice`, or unplug/replug).
`Status=Error` with problem code 52 means the test certificate isn't trusted or test-signing is off.

---

## 2. Run the probe

```powershell
.\nwcusb_probe_kmdf.exe <command> [channel] ["SSID"]
```

| Command | What it does |
|---|---|
| `ap-loop` | **The main one.** Full SoftAP: bring-up, beaconing, connector registration/grant, WEP auth, association, and an RX loop that logs and answers frames. |
| `beacon-once` | Bring the radio up and transmit a single beacon. |
| `dumpregs` | Dump EEPROM + every register bank (MAC/TXRX/SEC/PHY) + BBP registers. Best first command. |
| `rxdiag` | Receive-only diagnostic; decodes incoming frames. |
| `init-basic` | Register/soft-reset bring-up only. |
| `init-radio` | Basic init plus BBP/RF/channel/antenna. |
| `led-on` / `led-off` | Toggle the LED — quickest "am I actually talking to it" check. |
| `reset` / `usb-reset` | Device reset. |

Example — start an AP on channel 1 with a derived SSID:

```powershell
.\nwcusb_probe_kmdf.exe ap-loop 1
```

Expected sequence in the log: EEPROM/MAC read → BBP/RF init → AWAKE converges → beaconing →
`[connector] registration probe … -> GRANT` → `[auth] request … seq=1` → `[auth] response … seq=2`.
On this codebase the client then stalls at `seq1` (see the README).

---

## 3. Tuning knobs (environment variables)

These are the bench controls used to test hypotheses against real hardware. **This is the most
useful part of the repo if you're continuing the investigation** — most of the eliminated theories
were tested by flipping one of these, with no rebuild.

### Directly relevant to the auto-ACK problem

| Variable | Effect |
|---|---|
| `NWC_CSR19` | Force the final `TXRX_CSR19` value (TSF / `TSF_SYNC` / `TBCN` / `BEACON_GEN`). `0x1d` = the value the original driver leaves set; `0x05` = TSF only, no hardware beacon. |
| `NWC_CSR10` | Force a literal `TXRX_CSR10` (**auto-responder control**). Default is a read-modify-write that preserves the chip's power-on bits. |
| `NWC_SIFS` | Override SIFS (`MAC_CSR11`). Default 5. Used to test the "ACK fires too early/late" theory. |
| `NWC_APSTART` | Run the original driver's AP-activation arming edge: `TXRX_CSR2 = DISABLE_RX` → ~100 ms → re-enable, performed *after* the beacon engine is live. |
| `NWC_NOSWBEACON` | Disable the software beacon and rely solely on the hardware `BEACON_GEN`. **Required** if the hardware beacon is enabled — they collide on the chip's single TX engine. |
| `NWC_NORUNTIMELOOP` / `NWC_RTLOOP_MS` | Disable / set the interval of the runtime loop that re-asserts slot/SIFS/EIFS and dumps the `STA_CSR` statistics counters. |
| `NWC_RETRY7` | Restore `retry_limit = 7` in TX descriptors (default 0, matching the original driver). |
| `NWC_NOGUARDIAN` | Skip the 1-byte guardian transfer before normal frames. |
| `NWC_CSR20` | Override `TXRX_CSR20` (beacon TX offset; default `0x0140`). |

### Init / PHY variants

| Variable | Effect |
|---|---|
| `NWC_MATCHORIG` | Use the original driver's exact init ordering and power registers. |
| `NWC_OLDAWAKE` | Restore the earlier (pre-fix) AWAKE-transition ordering. |
| `NWC_KEEPCSR18` | Don't zero `MAC_CSR18` before the AWAKE transition. |
| `NWC_CSR1_AUTOSEQ` | Restore the `TXRX_CSR1` auto-sequence bit (dropped by default; absent in the original's capture). |
| `NWC_PHYCSR2` | Force a raw `PHY_CSR2` value. |
| `NWC_TXPOWER` | Override TX power (bench sweep). |

### Responder policy / debugging

| Variable | Effect |
|---|---|
| `NWC_KERNMAC` | Hand the MAC to the **in-kernel responder**: user mode configures the chip, then the driver owns beaconing and all probe/auth/assoc/grant responses. |
| `NWC_NODEDUP` | Answer every retransmitted auth `seq1` instead of de-duplicating. (De-duplication is on by default so repeated responses don't saturate the single TX engine.) |
| `NWC_ALLPROBE` | Answer *all* probe requests, not only ones addressed to our SSID. |
| `NWC_RXDUMP` | Dump raw received frames as hex. |

Example — the fullest reproduction of the original driver's AP activation:

```powershell
$env:NWC_APSTART = "1"
$env:NWC_NOSWBEACON = "1"     # hardware beacon only
$env:NWC_NORUNTIMELOOP = "1"
.\nwcusb_probe_kmdf.exe ap-loop 1
```

---

## 4. Offline driver harness (no radio needed)

`nwc_kmtest.exe` drives the driver's IOCTLs directly with synthetic frames (probe request, auth
`seq1`, WEP-protected `seq3`, association request, a retransmit burst) and polls the statistics
IOCTL. It exercises the in-kernel responder's parsing, WEP, and frame-building paths **without a
client or antenna** — this is how the responder was validated as crash-safe before ever being used
against hardware. Run it right after installing the driver.

---

## 5. Decoding a USB capture

```bash
python tools/decode_usb_capture.py capture.pcapng
```

Expects a **usbmon-format** (LinkType 220) pcapng. Produces `timeline.txt` — a chronological list
of register reads/writes (decoded to register names) and bulk 802.11 frames — and prints windows
around any authentication frames. This is how the original driver's behaviour was compared against
this implementation's.

---

## 6. Reading the results

Client error codes and what they mean here:

| Code | Meaning |
|---|---|
| `51301` / `51303` | Authentication failed — no hardware ACK for the client's `seq1`. **The open problem.** |
| `51099` | Associated, but no DHCP lease — the radio worked; fix internet sharing on the host. |
| `52103` | Connection test failed — usually stalled NAT on the sharing host. |

Reaching `51099` instead of `51303` means the radio side succeeded.
