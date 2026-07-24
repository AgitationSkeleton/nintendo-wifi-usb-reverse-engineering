F=/home/user/nwc/sniff_ds.pcap
DS=12:34:56:78:9a:bc
BSS=AA:BB:CC:DD:EE:FF
echo "=== ALL auth frames (0x0b), full detail ==="
echo "frame#   time         TA                 RA                 alg seq status"
tshark -r $F -Y "wlan.fc.type_subtype==0x0b" -T fields \
  -e frame.number -e frame.time_relative -e wlan.ta -e wlan.ra \
  -e wlan.fixed.auth.alg -e wlan.fixed.auth_seq -e wlan.fixed.status_code 2>/dev/null
echo
echo "=== auth + ACK interleaved chronologically (the handshake microstructure) ==="
echo "time         subtype TA                 RA                 seq"
tshark -r $F -Y "wlan.fc.type_subtype==0x0b || wlan.fc.type_subtype==0x1d" -T fields \
  -e frame.time_relative -e wlan.fc.type_subtype -e wlan.ta -e wlan.ra -e wlan.fixed.auth_seq \
  2>/dev/null | grep -A2 -B0 "0x000b" | head -50
echo
echo "=== every distinct frame subtype FROM our BSSID (what the RT2570 actually radiates) ==="
tshark -r $F -Y "wlan.ta==$BSS" -T fields -e wlan.fc.type_subtype 2>/dev/null | sort | uniq -c
echo "(0x0008=beacon 0x0005=proberesp 0x000b=auth 0x0001=assocresp 0x001d=ack)"
