cd /home/user/nwc
rm -f nwcusb_probe
gcc -O2 -Wall -Wno-unused-function -o nwcusb_probe nwcusb_probe.c -I. -I/usr/include/libusb-1.0 -lusb-1.0 2>&1 | grep -iE 'error' | head
[ -x ./nwcusb_probe ] || { echo BUILD FAILED; exit 1; }
echo "BUILD OK $(stat -c%s nwcusb_probe)b"
BSS=AA:BB:CC:DD:EE:FF; MON=wlx112233445566; iw dev "$MON" set channel 1 2>/dev/null
hwtest() {
  local label="$1"; shift
  DEV=$(basename "$(readlink -f /sys/class/net/wlxAABBCCDDEEFF/device 2>/dev/null)")
  [ -n "$DEV" ] && echo -n "$DEV" > /sys/bus/usb/drivers/rt2500usb/unbind 2>/dev/null
  pkill nwcusb_probe 2>/dev/null; sleep 1
  env "$@" setsid nohup ./nwcusb_probe ap-loop 1 > /tmp/hw.log 2>&1 < /dev/null &
  sleep 6
  local r=""; for i in 1 2 3; do timeout 4 tcpdump -i "$MON" -w /tmp/hw.pcap -q 2>/dev/null
    r="$r $(tshark -r /tmp/hw.pcap -Y "wlan.fc.type_subtype==0x08 && wlan.ta==$BSS" 2>/dev/null|wc -l)"; done
  echo "$label"
  grep -aE '\[hwbcn\]' /tmp/hw.log | head -1
  echo "   beacons/4s:$r   $(grep -aoE 'STA_CSR5 beacon-count=[0-9]+' /tmp/hw.log|tail -3|tr '\n' ' ')"
  pkill nwcusb_probe 2>/dev/null; sleep 1
}
hwtest "HW beacon buffer @0x2c00, CSR18=0x0640 CSR19=0x1f CSR20=0x4003" \
       NWC_HWBEACON=1 NWC_CSR18=0x0640 NWC_CSR19=0x001f NWC_CSR20=0x4003
hwtest "HW beacon buffer @0x2c00, ORIGINAL regs (CSR20=0x0140, CSR18 default)" \
       NWC_HWBEACON=1 NWC_CSR19=0x001f
