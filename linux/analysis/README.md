# Analysis tools (NUC)

Reusable scripts from the 2026-07-22 session. They run **on the NUC** (`user@192.0.2.10`) and
assume the layout used there: probe + captures under `/home/user/nwc/`, the RT2570 iface
`wlxAABBCCDDEEFF`, and the AR9271 monitor iface `wlx112233445566`. Adjust those constants for a
different rig. Most take a `.pcap` produced by `tcpdump -i <usbmon-bus|monitor-iface>`.

- **`regdecode-diff.sh`** — decode RT2570 register writes from two usbmon `.pcap`s
  (`ourcap.pcap` vs `rt2500cap.pcap`) and diff the beacon/TSF/auto-responder registers. This is
  how the CSR18/CSR19 differences were found. Uses `tshark 'usb.bmRequestType==0x40 &&
  usb.setup.bRequest==6'`, wIndex=offset, `usb.data_fragment`=value (LE).
- **`beacon-desc-diff.sh`** — dump the 20-byte TX descriptor + 802.11 header of beacon-sized bulk
  transfers from rt2500usb vs our probe (showed the descriptors are equivalent → the difference
  is the endpoint/queue, not the descriptor).
- **`hwbeacon-investigate.sh`** — enumerate all vendor writes by length and bulk-OUT transfers by
  endpoint/size (how we learned rt2500usb loads the beacon ~40x via bulk, not register writes).
- **`hwbeacon-radiation-test.sh`** — with software beacon OFF, does the hardware beacon engine
  radiate anything? (STA_CSR5 + on-air beacon count). Used to prove the HW beacon is dead.
- **`ota-auth-chronology.sh`** — over-the-air (AR9271) analysis: interleaved auth + ACK timeline,
  and the count of auth/proberesp/beacon frames actually radiated from our BSSID. This is the
  tool that proved the auto-ACK fires and seq2 never radiates.
- **`ota-analyze-seq2.sh`** — quick pass/fail: did seq2 radiate this run, did the DS advance to
  seq3/assoc.
- **`ds-test-harness.sh`** — arm a run: unbind rt2500usb, start the probe (edit the env knobs),
  put the AR9271 in monitor mode, start a timed capture. Then attempt the DS.
- **`fetch-rt2500usb-src.sh`** — pull the upstream rt2500usb/rt2x00 source (needs wget).

Captures live in `../captures/`; the reference driver source in `../rt2500usb-src/`.
