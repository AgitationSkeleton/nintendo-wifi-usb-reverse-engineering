#!/bin/sh
# Bring the Nintendo DS fully online on Wiimmfi through the RT2570 dongle + our userspace probe.
#
# This is the complete, working end-to-end path (verified 2026-07-24: DS reached "Connection
# successful" and authenticated to Wiimmfi via POST /ac):
#   connector grant -> WEP auth -> association -> DHCP -> DNS(Wiimmfi) -> NAT -> data bridge.
#
# Run as root on the NUC:  sudo ./start-datapath.sh
# Then on the DS: Nintendo WFC Setup -> Connect to your Nintendo Wi-Fi USB Connector -> test.
set -e
NWC=/home/user/nwc            # dir with the built ./nwcusb_probe
RT2570_IF=wlxAABBCCDDEEFF    # the Nintendo dongle's managed iface name
TAP=nwc0
UPLINK=eno1                  # NUC's internet-facing iface
LAN=192.168.44
DNS=164.132.44.106           # Wiimmfi DNS (RiiConnect24-hosted; redirects *.nintendowifi.net)

echo "[1/5] IP forwarding + NAT ($TAP -> $UPLINK)"
sysctl -w net.ipv4.ip_forward=1 >/dev/null
iptables -t nat -C POSTROUTING -o "$UPLINK" -j MASQUERADE 2>/dev/null || iptables -t nat -A POSTROUTING -o "$UPLINK" -j MASQUERADE
iptables -C FORWARD -i "$TAP" -o "$UPLINK" -j ACCEPT 2>/dev/null || iptables -A FORWARD -i "$TAP" -o "$UPLINK" -j ACCEPT
iptables -C FORWARD -i "$UPLINK" -o "$TAP" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || iptables -A FORWARD -i "$UPLINK" -o "$TAP" -m state --state RELATED,ESTABLISHED -j ACCEPT

echo "[2/5] TAP $TAP ($LAN.1/24)"
ip tuntap add dev "$TAP" mode tap 2>/dev/null || true
ip addr replace "$LAN.1/24" dev "$TAP"
ip link set "$TAP" up

echo "[3/5] free the RT2570 from the kernel driver"
DEV=$(basename "$(readlink -f /sys/class/net/$RT2570_IF/device 2>/dev/null)" 2>/dev/null || true)
[ -n "$DEV" ] && echo -n "$DEV" > /sys/bus/usb/drivers/rt2500usb/unbind 2>/dev/null || true
pkill nwcusb_probe 2>/dev/null || true; sleep 1

echo "[4/5] start the connector probe + data bridge"
# NWC_DATAPATH=1  -> attach the TAP and bridge DS<->internet data (software WEP)
# NWC_CSR0=0x1ec2 -> security register matched to the original XP driver
# NWC_PROBE_MINGAP_MS=300 -> throttle probe-responses so the TX engine stays clear
# (guardian-beacons-only, the seq2 fix, and the 1600B frame cap are compiled-in defaults now)
NWC_DATAPATH=1 NWC_CSR0=0x1ec2 NWC_PROBE_MINGAP_MS=300 \
  setsid nohup "$NWC/nwcusb_probe" ap-loop 1 > "$NWC/aploop_dp.log" 2>&1 < /dev/null &
sleep 8
grep -aE '\[bridge\] DS.*ENABLED|\[ap\] test AP active' "$NWC/aploop_dp.log" | tail -2 | sed 's/^/    /'

echo "[5/5] DHCP (dnsmasq, DHCP-only, hands out Wiimmfi DNS $DNS)"
mkdir -p /etc/dnsmasq.d
cat > /etc/dnsmasq.d/nwc.conf <<CONF
port=0
interface=$TAP
bind-dynamic
dhcp-range=$LAN.10,$LAN.100,255.255.255.0,12h
dhcp-option=3,$LAN.1
dhcp-option=6,$DNS,8.8.8.8
dhcp-authoritative
log-dhcp
CONF
pkill dnsmasq 2>/dev/null || true; sleep 1
dnsmasq --conf-file=/etc/dnsmasq.d/nwc.conf --pid-file=/run/nwc-dnsmasq.pid

echo
echo "READY. On the DS: Nintendo WFC Setup -> Connect to your Nintendo Wi-Fi USB Connector -> test."
echo "  probe log:   $NWC/aploop_dp.log      (grep '[bridge]' / '[auth]')"
echo "  DHCP leases: journalctl -t dnsmasq-dhcp -f"
echo "  bridge tap:  tcpdump -i $TAP"
