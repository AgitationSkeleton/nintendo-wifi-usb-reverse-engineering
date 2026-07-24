cd /home/user/nwc
dumpregs() {  # pcap -> "0xOFFSET VALUE LEN" lines (last write wins ordering preserved)
  tshark -r "$1" -Y "usb.bmRequestType==0x40 && usb.setup.bRequest==6" -T fields \
    -e usb.setup.wIndex -e usb.setup.wLength -e usb.data_fragment 2>/dev/null | \
  awk 'NF>=2{ off=sprintf("0x%04x",$1); val=$3; if(val=="")val="(len"$2")"; print off, val, $2 }'
}
regname() { case "$1" in
  0x0464)echo bcn_interval_CSR18;;0x0466)echo TSF_bcn_en_CSR19;;0x0468)echo tbtt_CSR20;;0x046a)echo CSR21;;
  0x041c)echo MAC_CSR14;;0x041e)echo MAC_CSR15;;0x0420)echo MAC_CSR16;;0x0422)echo MAC_CSR17;;0x0424)echo MAC_CSR18;;
  0x0446)echo autoresp_CSR3;;0x0448)echo autoresp_CSR4;;0x04ea)echo STA_CSR5_bcncount;;*)echo "";;esac; }

dumpregs ourcap.pcap | sort -u > /tmp/our.txt
dumpregs rt2500cap.pcap | sort -u > /tmp/rt.txt
echo "=== UNIQUE register OFFSETS written by rt2500usb but NEVER by our probe ==="
comm -13 <(awk '{print $1}' /tmp/our.txt|sort -u) <(awk '{print $1}' /tmp/rt.txt|sort -u) | while read o; do
  printf "  %s %-20s  rt2500 vals: %s\n" "$o" "$(regname $o)" "$(grep "^$o " /tmp/rt.txt|awk '{print $2}'|tr '\n' ',')"
done
echo
echo "=== beacon/TSF/autoresp registers: OUR value  vs  rt2500 value(s) ==="
for o in 0x0464 0x0466 0x0468 0x046a 0x041c 0x041e 0x0420 0x0422 0x0424 0x0446 0x0448 0x04ea; do
  ov=$(grep "^$o " /tmp/our.txt|awk '{print $2}'|tr '\n' ','); rv=$(grep "^$o " /tmp/rt.txt|awk '{print $2}'|tr '\n' ',')
  printf "  %s %-20s ours=[%s]  rt2500=[%s]\n" "$o" "$(regname $o)" "${ov:- MISSING}" "${rv:- none}"
done
echo
echo "=== large writes (>2 bytes) = frame/beacon uploads ==="
echo "-- ours --";   dumpregs ourcap.pcap   | awk '$3+0>2{print}' | head
echo "-- rt2500 --"; dumpregs rt2500cap.pcap | awk '$3+0>2{print}' | head
