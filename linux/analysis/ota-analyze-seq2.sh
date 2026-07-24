F=/home/user/nwc/sniff_bl.pcap
DS=12:34:56:78:9a:bc
BSS=AA:BB:CC:DD:EE:FF
pkill -f sniff_bl.pcap 2>/dev/null; sleep 2
echo "seq2 auth on air (ta=BSS):   $(tshark -r $F -Y "wlan.fc.type_subtype==0x0b && wlan.ta==$BSS" 2>/dev/null | wc -l)"
echo "seq2 log-attempts:           $(grep -cE '\[tx\] auth-response sent' /home/user/nwc/aploop_bl.log)"
echo "seq2 tx line (confirm ack=0): $(grep -aE '\[tx\] auth-response frame' /home/user/nwc/aploop_bl.log | head -1)"
echo "proberesp on air / sent:     $(tshark -r $F -Y "wlan.fc.type_subtype==0x05 && wlan.ta==$BSS" 2>/dev/null | wc -l) / $(grep -cE '\[tx\] probe-response sent' /home/user/nwc/aploop_bl.log)"
echo "beacons on air:              $(tshark -r $F -Y "wlan.fc.type_subtype==0x08 && wlan.ta==$BSS" 2>/dev/null | wc -l)"
echo "DS seq3 / assoc:             $(tshark -r $F -Y "wlan.fc.type_subtype==0x0b && wlan.ta==$DS && wlan.fixed.auth_seq==3" 2>/dev/null | wc -l) / $(tshark -r $F -Y "wlan.fc.type_subtype==0x00 && wlan.ta==$DS" 2>/dev/null | wc -l)"
