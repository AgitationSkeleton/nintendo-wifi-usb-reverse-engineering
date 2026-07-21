# Technical Notes — RT2570 SoftAP over USB

Everything below was derived by observing and reimplementing the hardware's behaviour. Register
names follow the open-source `rt2500usb` naming where they correspond; offsets were confirmed
against live hardware.

---

## 1. USB transport

The dongle exposes a single interface with two bulk endpoints and is driven almost entirely through
**vendor control transfers** on EP0.

| Endpoint | Direction | Use |
|---|---|---|
| `0x81` | IN (bulk) | received 802.11 frames (+ RX descriptor) |
| `0x01` | OUT (bulk) | transmitted frames (TX descriptor + frame), beacon-ring upload |

Both bulk endpoints report a 512-byte max packet size, i.e. the device enumerates at **USB 2.0 high
speed**. (Relevant if you virtualise it: a USB 1.1 controller forces full speed, where 512-byte
bulk endpoints are illegal and configuration selection fails.)

### Vendor requests

| `bRequest` | Direction | Meaning |
|---|---|---|
| `0x01` | OUT | write MAC address block |
| `0x02` | OUT | single register write — value in `wValue`, register in `wIndex` |
| `0x03` | IN  | single register read — register in `wIndex` |
| `0x06` | OUT | multi-register write |
| `0x07` | IN  | multi-register read |
| `0x09` | IN  | EEPROM read |

### Register banks

| Bank | Range | Contents |
|---|---|---|
| `MAC_CSR`  | `0x0400`–`0x042e` | station MAC, BSSID, max frame length, timers, slot/SIFS/EIFS, power state |
| `TXRX_CSR` | `0x0440`–`0x046a` | security, RX filter, auto-responder, ACK/CTS timing, TSF/beacon |
| `SEC_CSR`  | `0x0480`–`0x049e` | WEP key slots |
| `PHY_CSR`  | `0x04c0`–`0x04d4` | BBP and RF serial interfaces, antenna |
| `STA_CSR`  | `0x04e0`–`0x04f4` | statistics counters (cleared on read) |

Notable individual registers:

- `MAC_CSR2/3/4` — station MAC; `MAC_CSR5/6/7` — BSSID.
- `MAC_CSR10/11/12` — **slot time / SIFS / EIFS**. Observed operating values: `20 / 5 / 0x016c`.
- `MAC_CSR17` — current power state; AWAKE is polled for `& 0x1e0 == 0x1e0`.
- `MAC_CSR18` — wakeup-timer / beacon-delay fields; cleared before the AWAKE transition.
- `TXRX_CSR0` — **security** control (WEP algorithm / IV offset / key id), *not* an RX register.
- `TXRX_CSR2` — **RX filter**: `DISABLE_RX(0x01)`, `DROP_CRC(0x02)`, `DROP_PHYSICAL(0x04)`,
  `DROP_CONTROL(0x08)`, `DROP_NOT_TO_ME(0x10)`, `DROP_TODS(0x20)`, `DROP_VERSION_ERROR(0x40)`,
  `DROP_MULTICAST(0x200)`, `DROP_BROADCAST(0x400)`. Operating value observed: `0x0056`.
- `TXRX_CSR5-8` — auto-responder rate/BBP map for ACK and CTS.
- `TXRX_CSR10` — **auto-responder control**; only the `AUTORESPOND_PREAMBLE` bit (`0x04`) is
  publicly documented. The reference driver read-modify-writes *only* that bit and preserves the
  rest of the power-on value.
- `TXRX_CSR12-17` — ACK/CTS timing.
- `TXRX_CSR18` — beacon interval, `TXRX_CSR20` — beacon TX offset.
- `TXRX_CSR19` — **TSF/beacon control**: `TSF_COUNT(0x01)`, `TSF_SYNC(0x06)`, `TBCN(0x08)`,
  `BEACON_GEN(0x10)`. Full AP operating value: `0x001d`.
- `STA_CSR0` FCS errors, `STA_CSR3` false CCA, `STA_CSR5` beacon counter, `STA_CSR6-10` TX
  statistics (undocumented individually).

---

## 2. Bring-up sequence

Ordering matters; this is the sequence that reaches a working radio:

1. Soft reset via `MAC_CSR1`, then read the EEPROM (MAC address at offset `0x04`, per-channel TX
   power calibration, RF type).
2. BBP initialisation over the `PHY_CSR` serial interface (register/value pairs).
3. RF initialisation and channel programming (RF2525E and relatives), then antenna configuration.
4. Program station MAC (`MAC_CSR2/3/4`) and BSSID (`MAC_CSR5/6/7`).
5. Clear `MAC_CSR18`, then perform the **AWAKE** state transition late — after PHY/RF and after the
   MAC address is programmed — polling `MAC_CSR17` until the state converges.
6. Program RX filter, auto-responder registers, timing, and the WEP key.
7. Load the beacon and enable beacon generation (below).

---

## 3. TX / RX descriptors

- **TX (bulk OUT):** a **20-byte TX descriptor** precedes the 802.11 frame. Relevant word-0 bits
  include ACK-required, retry limit, **TIMESTAMP** (hardware inserts the live TSF — used for
  beacons), and the frame length. Management responses use ACK-required with a **retry limit of
  0**; beacons use TIMESTAMP with no ACK.
- **RX (bulk IN):** the 802.11 frame comes **first**, followed by a **16-byte RX descriptor
  trailer** carrying flags (to-me / multicast / broadcast / my-BSS / CRC error) and signal data.
- Every bulk-OUT frame is preceded by a **1-byte "guardian" transfer** on the same endpoint.

---

## 4. Hardware beacon (beacon ring)

The chip can emit beacons autonomously every TBTT, which keeps the TSF running without host
involvement. The working sequence is:

1. Disable `BEACON_GEN` (`TXRX_CSR19` with bit `0x10` clear) so the generator never reads a
   half-written ring.
2. Upload the beacon: guardian byte, then the 20-byte TX descriptor (**TIMESTAMP set, no ACK**) plus
   the beacon frame, on the bulk-OUT endpoint.
3. Set the beacon interval (`TXRX_CSR18`) and TX offset (`TXRX_CSR20 = 0x0140`).
4. Enable with an **alternating dance** — `0x001d → 0x0000 → 0x001d …` — because beacon generation
   fails on a single plain enable.

**Important practical finding:** the RT2570 has a **single TX engine**. Running a software beacon
(periodic bulk-OUT frames) *and* the hardware beacon simultaneously makes them collide, and the
client sees neither — the AP appears to vanish. Use one or the other. Once the hardware beacon is
loaded correctly and the software beacon is suppressed, a DS discovers the AP from the hardware
beacon alone.

---

## 5. The connector registration protocol

This is the proprietary part that makes a DS accept the dongle as an official connector, and the
part with no public documentation. It rides on ordinary probe request/response frames.

1. The DS broadcasts a **probe request** whose SSID information element carries a registration
   payload: an `NWCUSBAP`-style prefix, the console's user nickname (UTF-16LE), and a **control
   byte**.
2. The AP answers with a **probe response** whose SSID field encodes a **permission state** in a
   fixed byte position:
   - control `0x01` from the DS → AP replies "awaiting permission" (`0x20`)
   - once permission is granted → AP replies **GRANT** (`0x21`)
3. The DS, seeing the grant, proceeds to ordinary 802.11 authentication.

The original Windows software exposed this as a GUI prompt ("a console wants to connect — allow?").
This implementation auto-grants, which is the only intentional behavioural difference.

The beacon/probe SSID itself is derived deterministically from the dongle's MAC address.

---

## 6. WEP shared-key authentication

The DS uses **shared-key** authentication (`alg = 1`):

| Step | Direction | Contents |
|---|---|---|
| `seq1` | DS → AP | authentication request |
| `seq2` | AP → DS | challenge text |
| `seq3` | DS → AP | challenge encrypted with the WEP key |
| `seq4` | AP → DS | success |

Implementation notes:

- The WEP key is derived deterministically and programmed into hardware key slot 0 (`SEC_CSR`),
  with the key-valid bit set.
- On `seq3` the RT2570 **decrypts in place**: the Protected bit and the 4-byte IV remain in the
  frame, and the plaintext body begins at frame offset 28.
- Each `seq1` should be answered **once**; answering every retransmission floods the single TX
  engine.

At the MAC layer, `seq1` must additionally be **hardware-acknowledged** — see below.

---

## 7. The unresolved blocker: SIFS auto-ACK

After receiving a unicast frame addressed to it, the MAC must transmit an 802.11 ACK one SIFS
(~10 µs) later. The RT2570 does this **autonomously in hardware**; it cannot be produced in
software at that timescale. It is governed entirely by chip state (RX filter accepting the frame as
"to me", a running TSF, and the auto-responder being armed).

In this implementation the auto-ACK never fires, so the DS never advances past `seq1`
(client error 51303).

Configuration verified to match a working reference, and therefore **eliminated** as the cause:
RX filter and `DROP_NOT_TO_ME`; `TXRX_CSR19 = 0x001d` including the enable dance; auto-responder
control and rate map; slot/SIFS/EIFS timing (including continuously re-asserting them, as the
reference driver does at runtime); station MAC and BSSID; antenna; TX descriptors for management
responses; and a genuinely radiating hardware beacon with a live TSF.

Diagnostics run from the host cannot distinguish "no ACK is transmitted" from "an ACK is
transmitted that the client cannot decode", because the ACK never crosses the USB bus. Statistics
counters (`STA_CSR6-10`) did not move in step with transmitted management responses, which is
consistent with the frames not being tracked as ordinary acknowledged transmissions, but is not
conclusive.

**Resolving this requires an over-the-air monitor-mode capture.** That single measurement splits the
problem into two mutually exclusive, separately tractable failures.

---

## 8. Client error codes seen

| Code | Meaning in this context |
|---|---|
| `51301` / `51303` | authentication failed — no hardware ACK for `seq1` (the blocker here) |
| `51099` | associated successfully, but no DHCP lease (host-side sharing/NAT problem) |
| `52103` | connection test failed (typically stalled NAT on the sharing host) |

Reaching `51099` rather than `51303` means association succeeded and the remaining problem is the
host's internet-sharing configuration, not the radio.
