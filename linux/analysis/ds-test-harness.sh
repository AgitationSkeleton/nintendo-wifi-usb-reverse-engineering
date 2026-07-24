cd /home/user/nwc
BSS=AA:BB:CC:DD:EE:FF
DEV=$(basename "$(readlink -f /sys/class/net/wlxAABBCCDDEEFF/device 2>/dev/null)")
[ -n "$DEV" ] && echo -n "$DEV" > /sys/bus/usb/drivers/rt2500usb/unbind 2>/dev/null
pkill nwcusb_probe 2>/dev/null; sleep 1
NWC_CSR19=0x000f NWC_PROBE_MINGAP_MS=300 \
  setsid nohup ./nwcusb_probe ap-loop 1 > /home/user/nwc/aploop_ap5.log 2>&1 < /dev/null &
sleep 8
grep -aE '\[ap\] test AP active' aploop_ap5.log | tail -1 || { echo "PROBE FAIL"; tail -6 aploop_ap5.log; exit 1; }
MON=wlx112233445566
iw dev "$MON" info 2>/dev/null | grep -q monitor || { ip link set "$MON" down; iw dev "$MON" set type monitor; ip link set "$MON" up; }
iw dev "$MON" set channel 1
pkill -f sniff_ap5.pcap 2>/dev/null; sleep 1
setsid nohup timeout 170 tcpdump -i "$MON" -w /home/user/nwc/sniff_ap5.pcap -q >/dev/null 2>&1 < /dev/null &
sleep 1
echo ">>> ARMED 170s: AP-mode TSF_SYNC=3 (CSR19=0x0f, no BEACON_GEN) — attempt DS now"
