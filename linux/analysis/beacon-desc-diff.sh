cd /home/user/nwc
echo "=== rt2500usb beacon-sized bulk-OUT payloads (93/98/104B) — TX desc + 802.11 hdr ==="
tshark -r rt2500cap.pcap -Y "usb.transfer_type==0x03 && usb.endpoint_address==0x01 && usb.data_len>=90 && usb.data_len<=110" \
  -T fields -e usb.data_len -e usb.capdata 2>/dev/null | sort -u | head -4 | while read len data; do
  echo "len=$len"
  echo "  desc(0-19): $(echo $data | tr -d ':' | cut -c1-40)"
  echo "  frame(20+): $(echo $data | tr -d ':' | cut -c41-96)"
done
echo
echo "=== OUR beacon bulk-OUT payloads (100/106B) — TX desc + 802.11 hdr ==="
tshark -r ourcap.pcap -Y "usb.transfer_type==0x03 && usb.endpoint_address==0x01 && usb.data_len>=98 && usb.data_len<=110" \
  -T fields -e usb.data_len -e usb.capdata 2>/dev/null | sort -u | head -3 | while read len data; do
  echo "len=$len"
  echo "  desc(0-19): $(echo $data | tr -d ':' | cut -c1-40)"
  echo "  frame(20+): $(echo $data | tr -d ':' | cut -c41-96)"
done
