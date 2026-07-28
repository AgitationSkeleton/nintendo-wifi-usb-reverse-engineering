# Windows-native pipeline — getting a DS/3DS onto Wiimmfi from Windows

The same `src/probe/nwcusb_probe.c` that drives the Linux pipeline also runs natively on Windows
(`#ifdef _WIN32`). Where Linux leans on the kernel (`rt2500usb` for WEP/TSF, a TAP device, and
`iptables` MASQUERADE), Windows has none of that, so the probe brings its own userspace equivalents:

| Concern | Linux | Windows (userspace, in-probe) |
|---|---|---|
| USB | usbfs (microsecond transfers) | libusbK (bulk async pool + a dedicated TX thread) |
| WEP RX/TX | RT2570 hardware engine | software RC4 + CRC-32 ICV |
| L3 interface | TAP | Wintun |
| NAT | kernel conntrack MASQUERADE | WinDivert full-NAT (SNAT + in-order return delivery) |
| DHCP/DNS/ARP | dnsmasq / kernel | answered in-probe |

## Build (Windows)
Compile the single translation unit against libusb-1.0, WinDivert, and ws2_32:

```
cl /O2 /I <headers> nwcusb_probe.c /Fe:nwcusb_probe.exe /link libusb-1.0.lib ws2_32.lib WinDivert.lib
```

Ship `WinDivert.dll` + `WinDivert64.sys`, `libusb-1.0.dll`, and `wintun.dll` beside the exe. The
dongle must be bound to a libusb-compatible driver (WinUSB/libusbK via Zadig). Run elevated
(WinDivert needs it).

## Why the naive Windows port crashed the DS, and the fixes

On a *quiet* Windows box the port works. On a *loaded* one the failures below appear — each is a
place where the Windows userspace path behaves differently from the Linux kernel path. The DS's
retail GameSpy/networking stack is unforgiving: lose or mis-order the wrong packet and it either
disconnects or dereferences a wild pointer (ARM Data Abort).

1. **A dedicated USB-TX thread.** libusbK sync bulk transfers on the main loop stall it under host
   load, starving the ~100 ms software beacon. Route all bulk-OUT through one thread that owns the
   single TX engine and re-sends the beacon on a heartbeat, so beacon cadence holds through a data
   flood. Give management frames (auth/assoc/probe-resp) a priority tier **above** data — otherwise
   the auth-response waits behind a QR2 flood, the DS times out and re-authenticates in a storm.

2. **Beacon interval register.** The rt2500usb value is 100 TU; a wrong computation gave 400 TU, so
   the hardware fired every ~410 ms while the beacon frame advertised ~102 ms → constant "missed
   beacon" darking. Set `TXRX_CSR18` to the 100 TU value.

3. **Don't do diagnostic control transfers on the hot path.** A periodic hardware-beacon re-arm that
   issued a burst of libusbK *synchronous* control transfers on the main loop stalled it ~700 ms
   under load → beacon blackouts >1 s → the DS lost frames *and* its ACKs → the server retransmitted
   3–8× → slow connect → matchmaking timeout. With the TX-thread heartbeat already carrying the
   beacon, re-assert TSF only every ~20 s and skip diagnostic register reads. (Linux does TSF/beacon
   in-kernel with no per-cycle USB control transfers — this is a pure Windows-path cost.)

4. **Full-NAT return path, keyed correctly.** WinDivert captures replies to the WAN IP and un-NATs
   them to the DS. The SNAT preserves the DS's source port, so the reply already carries the correct
   destination port — do **not** overwrite it from a NAT table keyed only by (server-ip, server-port).
   Many simultaneous connections share one server port (e.g. the GameSpy GP backend), so such a table
   holds only the newest connection's port and mis-delivers every other connection's replies to the
   wrong socket → the DS gets data on an unexpected connection → hang/crash. Kernel conntrack tracks
   the full tuple and never confuses them; the userspace path must too.

5. **Re-deliver only the login handshake, and stop on FIN/RST.** Small login segments (the GPCM
   challenge / login result) are worth re-delivering until the DS ACKs, because they are lossy over
   the air and slow to retransmit. But re-delivering *non-login* segments onto the short-lived
   backend connections that open and close rapidly during matchmaking pushes stale data onto sockets
   the DS is tearing down → freed-pointer crash. Confine re-delivery to the GPCM/GPSP login ports and
   drop the cached segment the instant a FIN/RST touches its connection.

6. **Complete the server IP set.** The DS resolves several Wiimmfi hostnames; a strict source-IP
   capture filter must include *all* of them (NAS/conntest, GPCM/GPSP, master, natneg, and
   **gamestats**). Missing one silently drops that flow — e.g. omitting gamestats drops the post-race
   stats report and hangs the DS's networking a few minutes into worldwide play.

7. **Worldwide P2P needs the peer return path.** Worldwide matchmaking hole-punches to *arbitrary*
   peer IPs over UDP, not just the fixed Wiimmfi servers. A server-IPs-only filter drops the peer
   replies → strict NAT → error 86420. Capture inbound UDP to the WAN IP so peer replies reach the
   DS. On a host that also runs other UDP services, exclude those services' ports from the capture
   (a configurable exclusion list) so only the DS's own ephemeral-port peer replies are taken and the
   host's services are never touched. With that, the DS reaches the GameSpy Transport 2 (GT2) peer
   handshake and a real match lobby.

## Reading Wiimmfi error codes as a progress ladder
`crash` → `52xxx` (return path) → **86420** (NAT traversal) → **87080** (GT2 P2P, non-fatal) →
**91010** (GPCM disconnect, non-fatal). Each later code means the connection got further before
failing. The last mile is P2P/GPCM link margin at race-start: the DS storm-retransmits when peer
replies are lossy, saturating the air link and starving the background GPCM keepalive.

## Root-cause summary
Every crash traced to a userspace-path difference from the Linux kernel — sync USB control transfers
on the hot path, software beacon starvation, a NAT table that collides connections sharing a server
port, and re-delivery with no connection-state awareness. None required kernel drivers to fix; they
are all in the single probe source.
