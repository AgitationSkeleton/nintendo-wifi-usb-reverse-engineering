# Nintendo Wi-Fi USB Connector — Reverse Engineering (Unfinished)

A near-complete reverse engineering and reimplementation of the **Nintendo Wi-Fi USB Connector**'s
SoftAP stack for modern Windows — which gets a real Nintendo DS *almost* all the way online, and
then stops at exactly one hardware behaviour we could never reproduce.

**This project does not work end-to-end. It is published as a detailed record of how far it got,
what was proven, and precisely what is left.** If you are looking for a working solution today,
see [Does anything work?](#does-anything-work) below.

---

## The hardware

The Nintendo Wi-Fi USB Connector is a rebadged Buffalo USB Wi-Fi dongle built on the **Ralink
RT2570** chipset (USB `VID 0x0411` / `PID 0x008B`). It is not a normal Wi-Fi adapter: bundled
Windows XP-era software turns it into a **SoftAP** that speaks a small proprietary "registration"
protocol, which a Nintendo DS recognises as an official connector and uses for Nintendo Wi-Fi
Connection.

The official software is long abandoned and does not install or run on modern Windows. The goal
here was a clean, modern Windows (10/11) replacement driving the same physical hardware.

---

## Status: how close it got

Everything in the connection sequence works **except the final hardware acknowledgement**:

| Stage | Status |
|---|---|
| USB register surface (control-transfer register map) | ✅ fully reverse engineered |
| Dongle bring-up: EEPROM, MAC/BBP/RF init, channel + antenna, AWAKE state machine | ✅ working |
| Software beaconing | ✅ working |
| **Hardware** beaconing (beacon-ring upload + `BEACON_GEN`, autonomous TBTT beacons) | ✅ working — DS discovers the AP from it |
| Proprietary connector **registration / permission-grant** handshake | ✅ fully reverse engineered and reimplemented — the DS is admitted |
| WEP shared-key auth: challenge, hardware key programming, RC4/CRC32, `seq2`/`seq4` | ✅ implemented, DS accepts our `seq2` |
| Association request/response | ✅ implemented |
| **802.11 SIFS auto-ACK of the DS's auth `seq1`** | ❌ **never fires — this is the blocker** |
| DHCP / NAT / internet path for the DS | ⛔ never reached |

A real DS **finds the AP, completes the registration handshake, is granted permission, and begins
WEP authentication** — then stalls and reports **error 51303**.

---

## Why it failed

### The blocker in one paragraph

When the DS transmits its authentication frame (`seq1`), the access point's MAC must reply with an
802.11 ACK roughly **10 microseconds** later (one SIFS). This is emitted **autonomously by the
RT2570's MAC hardware** — no software can do it, because 10 µs is orders of magnitude below any
USB round-trip or interrupt latency. Whether it happens is purely a function of how the chip's
registers are configured. **We were never able to get that auto-responder to fire.** The DS,
seeing no acknowledgement, retransmits `seq1` (Retry bit set), never advances, and times out.

### What was ruled out

Each of these was implemented and tested against a real DS. All still produced 51303:

- **Responder latency.** The entire 802.11 responder was moved into a kernel driver (from
  user-mode). No change — correctly so, since the auto-ACK is below the software layer entirely.
- **`TXRX_CSR19`** (TSF / beacon / `TBCN` / `BEACON_GEN`). Tested at the exact value the original
  driver leaves set (`0x001d`), including the alternating enable "dance". No change.
- **RX filter (`TXRX_CSR2`, incl. `DROP_NOT_TO_ME`).** Matches the original byte-for-byte. A
  theory that the original ran promiscuously was tested and disproven.
- **Auto-responder control (`TXRX_CSR10`)**, auto-responder rate map (`TXRX_CSR5-8`), timing
  registers (slot / **SIFS** / EIFS), antenna configuration, BSSID/MAC registers — all match.
- **The original driver's runtime loop.** It continuously re-asserts the slot/SIFS/EIFS timing
  registers after init; replicating that changed nothing.
- **A genuinely radiating hardware beacon.** The theory that the auto-responder only arms once the
  hardware TSF/beacon engine is truly running was the most promising lead. We eventually *did* get
  a real hardware beacon radiating (the DS discovers and authenticates against it) — and the
  auto-ACK *still* did not fire.

### A correction worth recording

An early assumption was that "the DS retransmits `seq1` with the Retry bit" *is* the failure
signature. Decoding a USB capture of the **original driver working** showed the DS retransmits
`seq1` there too (roughly seven times). The real difference is that in the working case it then
**advances to `seq3`**. So the correct failure signature is *"never reaches `seq3`"*, not
*"retransmits `seq1`"*.

### Why we could not diagnose further

The auto-ACK is generated **inside the chip's MAC and never crosses the USB bus**. Therefore no USB
capture can show it — including captures of the original driver succeeding. From the host side,
these two situations are **indistinguishable** but need opposite fixes:

1. The auto-responder is disarmed and no ACK is transmitted at all.
2. An ACK *is* transmitted, but the DS cannot decode it (wrong rate, preamble, or timing).

Separating them requires an **over-the-air monitor-mode capture** during a DS authentication
attempt. That hardware was not available, so the investigation stopped there rather than guessing
between two opposite fixes.

---

## What this project accomplishes

Even unfinished, the useful results are:

- **A complete, documented register-level map** of driving an RT2570 as a SoftAP over USB —
  bring-up, PHY/RF/BBP programming, beaconing, WEP key programming, and the TX/RX descriptor
  formats.
- **A full reimplementation of the proprietary connector registration protocol** — the piece that
  makes a DS treat the dongle as an official Nintendo connector. This is the part with no public
  documentation, and it works.
- **A working hardware beacon path** on the RT2570 over USB (beacon-ring upload with the guardian
  byte, plus the `BEACON_GEN` enable sequence), including the discovery that a software beacon and
  the hardware beacon collide on the chip's single TX engine.
- **Two complete implementations** to build on: a user-mode SoftAP and a KMDF kernel driver with an
  in-kernel 802.11 responder.
- **A precise, evidence-backed statement of the one remaining unknown**, plus a list of hypotheses
  already eliminated — so nobody has to re-walk them.

---

## Repository layout

```
src/probe/    user-mode SoftAP implementation (the bulk of the reverse engineering)
src/driver/   KMDF kernel driver + in-kernel 802.11 responder, build/sign scripts
tools/        decoder for USB captures (usbmon-format pcapng) into a register/frame timeline
docs/         documentation (below)
```

| Document | Contents |
|---|---|
| [docs/BUILDING.md](docs/BUILDING.md) | Prerequisites (MSVC / SDK / WDK / libusb header), build steps, test-signing, and the USB-controller requirement when virtualising the dongle. |
| [docs/USAGE.md](docs/USAGE.md) | Installing the driver, the probe's commands, the **~20 environment-variable tuning knobs** used to test hypotheses without rebuilding, the offline driver harness, and how to read the client error codes. |
| [docs/TECHNICAL.md](docs/TECHNICAL.md) | Register map, bring-up ordering, TX/RX descriptor formats, the hardware beacon-ring mechanism, the connector registration protocol, WEP auth, and the auto-ACK analysis. |
| [docs/reference/original-driver-init-sequence.txt](docs/reference/original-driver-init-sequence.txt) | Our decoded ~3,680-step register/USB init sequence of the **original vendor driver** — the ground truth this implementation was compared against. |

Build scripts target the Windows WDK/SDK and MSVC. The kernel driver requires test-signing to load
— **use a throwaway VM**, not a machine you care about.

If you are picking this up, the fastest orientation is: `docs/USAGE.md` (the tuning knobs) →
`docs/TECHNICAL.md` §7 (the unresolved blocker) → the reference init sequence.

---

## If you want to pick this up

The single highest-value experiment: **capture the air with a monitor-mode adapter while a DS
attempts to authenticate**, and answer one question — *does an ACK appear after the DS's `seq1`?*

- **No ACK on air** → the auto-responder is disarmed; keep hunting chip state.
- **ACK on air, DS ignores it** → it is a rate/preamble/timing mismatch on the ACK frame, which is
  a different and likely tractable problem.

Almost any monitor-capable adapter (or a Linux live USB) is sufficient. Everything else needed to
reproduce the setup is in this repository.

---

## Does anything work?

Yes — but not this code. The **original Windows XP software, run inside a virtual machine** with
the dongle passed through, still works and can get a DS online against community revival servers.
That remains the practical solution today. This project was an attempt to replace it natively, and
it did not get there.

---

## Scope, legality, and attribution

- This repository contains **only original code and observations**. No vendor drivers, firmware,
  executables, installers, or packet captures are included or redistributed.
- Reverse engineering here was for **interoperability** with hardware that is out of support, with
  no official software available for current operating systems.
- Personal identifiers (hardware addresses, console nicknames, machine names, network details) have
  been removed; MAC addresses in the source are non-routable placeholders, and the real values are
  read from the dongle's EEPROM at runtime.
- *Nintendo*, *Nintendo DS*, and *Nintendo Wi-Fi Connection* are trademarks of Nintendo. *Buffalo*
  and *Ralink*/*MediaTek* are trademarks of their respective owners. Used nominatively for
  identification only. This project is **not affiliated with, endorsed by, or supported by** any of
  them.
- Nintendo's official Wi-Fi Connection service was discontinued in 2014; this work targets
  community-run revival servers.

Licensed under the MIT License — see [LICENSE](LICENSE).
