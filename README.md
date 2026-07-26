# Nintendo Wi-Fi USB Connector — Reverse Engineering

A clean-room reverse engineering and reimplementation of the **Nintendo Wi-Fi USB Connector**'s
SoftAP stack — which now gets a **real Nintendo DS fully online on the Wiimmfi revival service**,
driving the original dongle entirely from **userspace on Linux** (no vendor driver, no Windows XP,
no virtual machine).

**This works end to end.** A DS finds the access point, completes the proprietary connector
registration, authenticates (WEP shared-key), associates, gets an IP by DHCP, and its game traffic
is bridged and NAT'd out to Wiimmfi — verified by the console reaching "Connection successful" and
appearing in Wiimmfi's live matchmaking list.

> This started as a Windows reimplementation that got *almost* all the way and stalled at what
> looked like an unreproducible hardware behaviour. That turned out to be a **misdiagnosis**. Moving
> to Linux with an over-the-air monitor made the real cause visible — it was a one-byte USB framing
> quirk, not a hardware wall — and from there the whole path came together. The corrected story is
> below.

---

## Quick start — download & run

Prebuilt, ready-to-run builds are published on the [Releases](../../releases) page (one Windows
build, one Linux build), produced by [`.github/workflows/release.yml`](.github/workflows/release.yml).

You need a Ralink **RT2570** dongle (USB `0411:008B` — the original connector hardware) and a DS.

**Windows 10/11**
1. Download **`nwc-connector.exe`** (a single self-contained executable — the probe engine, Wintun,
   and libusb are embedded and unpacked on first run).
2. One time per dongle, bind it to WinUSB/libusbK: run `install-driver.cmd` (from the `.zip`) or
   [Zadig](https://zadig.akeo.ie/) → select `0411:008B` → install WinUSB.
3. Run `nwc-connector.exe`. It self-elevates, opens a console with live logging, creates the
   `NWC-DS` adapter and NAT, and starts the AP. Optional: `nwc-connector.exe --install-autostart`.
4. On the DS: *Nintendo Wi-Fi Connection Setup → Connect to your Nintendo Wi-Fi USB Connector*.

**Linux**
1. Download and extract `nwc-connector-linux-x86_64.tar.gz`.
2. `sudo ./nwc-connector.sh` — installs any missing dependencies, auto-detects the dongle and the
   internet uplink, brings up the TAP/NAT and a Wiimmfi-redirecting DNS, and starts the probe.

> Both paths get a DS to **Wiimmfi matchmaking**. Linux (kernel NAT) is the reference and the most
> consistent. The Windows build is a full native port of the same probe engine — it answers DHCP/DNS
> in-process and NATs the gamespy/matchmaking return traffic through **WinDivert** (userspace),
> because Windows' built-in NAT silently drops some gamespy replies. Windows is more sensitive to
> 2.4 GHz congestion and to a VPN capturing the WAN IP, so keep the dongle near the DS, prefer a
> quiet Wi-Fi channel 1, and split-tunnel any VPN.

---

## The hardware

The Nintendo Wi-Fi USB Connector is a rebadged Buffalo USB Wi-Fi dongle built on the **Ralink
RT2570** chipset (USB `VID 0x0411` / `PID 0x008B`). It is not a normal Wi-Fi adapter: the bundled
Windows XP-era software turns it into a **SoftAP** that speaks a small proprietary "registration"
protocol, which a Nintendo DS recognises as an official connector and uses for Nintendo Wi-Fi
Connection. The official software is long abandoned and does not run on modern systems.

You also need, for the Linux path:
- a Linux host with a wired uplink (a small NUC/box works well),
- the RT2570 dongle (the connector itself),
- optionally a second monitor-mode adapter (e.g. AR9271 / `ath9k_htc`) — invaluable for debugging,
  and how the real cause was found.

---

## Status: it works

| Stage | Status |
|---|---|
| USB register surface (control-transfer register map) | ✅ fully reverse engineered |
| Dongle bring-up: EEPROM, MAC/BBP/RF init, channel + antenna, AWAKE state machine | ✅ |
| Software beaconing (discovery) | ✅ |
| Proprietary connector **registration / permission-grant** handshake | ✅ reimplemented — DS admitted |
| WEP shared-key auth (`seq1`→`seq2`→`seq3`→`seq4`, RC4/CRC32, HW key programming) | ✅ DS authenticates |
| Association | ✅ |
| **802.11 SIFS auto-ACK of the DS's auth `seq1`** | ✅ fires in hardware (verified over the air) |
| **Data bridge**: DS data ⇄ TAP ⇄ DHCP + NAT (software WEP) | ✅ |
| **DS fully online on Wiimmfi** (DHCP, DNS, conntest, NAS login) | ✅ **verified** |

---

## How it works, end to end

The Linux program (`src/probe/nwcusb_probe.c`) opens the dongle over **libusb** and acts as the
whole access point in userspace:

1. **Bring-up & beacon** — initialises the RT2570 (EEPROM/MAC/BBP/RF, channel, antenna) and beacons
   the connector SSID so the DS discovers it.
2. **Connector grant** — answers the DS's proprietary registration probe with the permission-grant
   reply, so the DS treats the dongle as an official Nintendo connector.
3. **WEP auth & association** — derives the connector's WEP key from the SSID, programs the hardware
   key, and completes shared-key authentication (`seq2` challenge / `seq4` success) and association.
4. **Data bridge** (`NWC_DATAPATH=1`) — attaches a **TAP** interface and bridges the DS's traffic:
   received data frames are WEP-decrypted (the RT2570 decrypts in place; the code reads the
   hardware plaintext) and written to the TAP as Ethernet; replies from the TAP are re-framed as
   802.11, **software WEP-encrypted**, and transmitted back to the DS.
5. **Internet / Wiimmfi** — on the Linux side, `dnsmasq` serves DHCP on the TAP (handing out the
   public Wiimmfi DNS) and `iptables` MASQUERADEs the DS's traffic out the wired uplink. The DS's
   DNS, connection test, and NAS login all reach Wiimmfi.

One command brings the whole thing up: [`linux/start-datapath.sh`](linux/start-datapath.sh).

---

## The corrected story (what actually blocked it, and the fixes)

Three findings turned "almost" into "working":

**1. The auto-ACK was never the problem — a one-byte USB guardian was.**
The long-standing theory was that the RT2570's ~10 µs SIFS auto-ACK of the DS's `seq1` never fired.
An over-the-air monitor capture of our *own* access point disproved it: **the hardware auto-ACK
fires every time.** The real wall was that our `seq2` auth-response never left the antenna. Decoding
the original driver's USB capture showed why: we prepended a **1-byte "guardian" transfer to every
frame**, but the original driver guardians **only the beacon** — data and management frames
(including `seq2`) are sent as a bare bulk transfer. Sending the guardian before `seq2` derailed
that transmit. Guardian-beacons-only → `seq2` radiates → the DS authenticates and associates.

**2. Received data is already hardware-decrypted.**
The RT2570 WEP-decrypts received frames *in place*; the plaintext sits just past the IV/key-id.
Software-decrypting it again produced ICV mismatches. The bridge reads the hardware plaintext
directly (with a software-decrypt fallback).

**3. A 512-byte transmit cap was silently dropping the connection-test response.**
The transmit path rejected frames over 512 bytes, so the ~536-byte HTTP connection-test reply never
reached the DS — the console held "green" for the whole test and failed only at the very last step.
Raising the cap fixed it, and the DS completes.

The over-the-air monitor adapter was the key tool: it made the difference between "no ACK on air"
and "ACK on air, ignored" observable, which is what redirected the whole investigation.

---

## Quick start (Linux)

```sh
# 1. Build the userspace probe (needs gcc + libusb-1.0 dev headers)
cd src/probe && ./build.sh

# 2. Bring up the full path as root (frees the dongle from the kernel driver,
#    sets up the TAP + dnsmasq DHCP + NAT, starts the probe with the data bridge).
#    Edit the interface names / uplink at the top of the script for your machine first.
sudo linux/start-datapath.sh

# 3. On the DS: Nintendo WFC Setup -> Connect to your Nintendo Wi-Fi USB Connector -> test.
```

The bring-up script's runtime state (TAP, dnsmasq, NAT) is not reboot-persistent — re-run it after
a reboot. See [docs/LINUX_PIPELINE.md](docs/LINUX_PIPELINE.md) for the full account and the
`NWC_*` tuning knobs.

---

## Repository layout

```
src/probe/    userspace SoftAP + connector protocol + data bridge (the core; builds on Linux and Windows)
src/driver/   an alternative KMDF kernel driver + in-kernel 802.11 responder (Windows path)
linux/        the Linux pipeline: one-shot bring-up, NUC setup, sniffer, and analysis tools
tools/        decoder for USB captures (usbmon-format pcapng) into a register/frame timeline
docs/         documentation (below)
```

| Document | Contents |
|---|---|
| [docs/LINUX_PIPELINE.md](docs/LINUX_PIPELINE.md) | The full narrative: the guardian-byte breakthrough, the data bridge, and the fixes that got the DS online. |
| [docs/TECHNICAL.md](docs/TECHNICAL.md) | Register map, bring-up ordering, TX/RX descriptor formats, connector registration, WEP auth. |
| [docs/rt2500usb-beacon-notes.md](docs/rt2500usb-beacon-notes.md) | Annotated upstream `rt2500usb` beacon/guardian mechanism (the reference that explained the guardian). |
| [docs/BUILDING.md](docs/BUILDING.md) · [docs/USAGE.md](docs/USAGE.md) | Windows build + the ~20 `NWC_*` tuning knobs and client error codes. |
| [docs/reference/original-driver-init-sequence.txt](docs/reference/original-driver-init-sequence.txt) | Decoded ~3,680-step register/USB init of the **original vendor driver** — the ground truth. |
| [linux/analysis/README.md](linux/analysis/README.md) | The usbmon register-diff and over-the-air sniff tools used throughout. |

---

## Scope, legality, and attribution

- This repository contains **only original code and observations**. No vendor drivers, firmware,
  executables, installers, or packet captures are included or redistributed. (The analysis tools
  reference captures that are **not** published — they contain personal network data.)
- Reverse engineering here was for **interoperability** with hardware that is out of support, with
  no official software available for current operating systems.
- Personal identifiers (hardware addresses, console nicknames, machine names, network details) have
  been removed; MAC/IP values in the source and scripts are non-routable placeholders — replace them
  with your own, and the dongle's real MAC is read from its EEPROM at runtime.
- *Nintendo*, *Nintendo DS*, and *Nintendo Wi-Fi Connection* are trademarks of Nintendo. *Buffalo*
  and *Ralink*/*MediaTek* are trademarks of their respective owners. Used nominatively for
  identification only. This project is **not affiliated with, endorsed by, or supported by** any of
  them.
- Nintendo's official Wi-Fi Connection service was discontinued in 2014; this work targets
  community-run revival servers (Wiimmfi).

Licensed under the MIT License — see [LICENSE](LICENSE).
