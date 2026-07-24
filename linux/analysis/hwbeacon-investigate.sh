cd /home/user/nwc
echo "=== ALL vendor writes (bRequest=6) in rt2500cap by wLength (find the beacon load) ==="
tshark -r rt2500cap.pcap -Y "usb.bmRequestType==0x40 && usb.setup.bRequest==6" -T fields \
  -e usb.setup.wIndex -e usb.setup.wLength 2>/dev/null | \
  awk '{printf "0x%04x  len=%s\n",$1,$2}' | sort | uniq -c | sort -rn -k3 | head -25
echo
echo "=== bulk-OUT transfers in rt2500cap (beacon may go via bulk endpoint) ==="
tshark -r rt2500cap.pcap -Y "usb.transfer_type==0x03 && usb.endpoint_address.direction==0" -T fields \
  -e usb.endpoint_address -e usb.data_len 2>/dev/null | sort | uniq -c | sort -rn | head -10
echo
echo "=== for comparison, OUR capture bulk-OUT (software beacon path) ==="
tshark -r ourcap.pcap -Y "usb.transfer_type==0x03 && usb.endpoint_address.direction==0" -T fields \
  -e usb.endpoint_address -e usb.data_len 2>/dev/null | sort | uniq -c | sort -rn | head -6
echo
echo "=== biggest single vendor write in rt2500cap (offset + length + first bytes) ==="
tshark -r rt2500cap.pcap -Y "usb.bmRequestType==0x40 && usb.setup.bRequest==6 && usb.setup.wLength>2" -T fields \
  -e usb.setup.wIndex -e usb.setup.wLength -e usb.data_fragment 2>/dev/null | \
  awk '{printf "0x%04x len=%s data=%s\n",$1,$2,substr($3,1,40)}' | sort -u | head -20
