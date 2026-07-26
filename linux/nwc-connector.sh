#!/usr/bin/env bash
# =============================================================================
#  connector.sh - all-in-one Nintendo DS -> Wiimmfi connector for Linux.
#
#  Turns an RT2570 "Nintendo Wi-Fi USB Connector" dongle into a live Wiimmfi
#  access point so a Nintendo DS can play Mario Kart DS / Metroid Prime Hunters
#  etc. online. Portable + turnkey: auto-detects the dongle and your uplink,
#  installs missing packages, builds the probe if needed, and brings up the full
#  data path (TAP + NAT + DHCP/DNS). Leave it running; Ctrl+C stops + cleans up.
#
#  Works with ANY genuine Nintendo Wi-Fi USB Connector (USB 0411:008b) - the
#  probe auto-reads the dongle's own MAC, so there is nothing per-dongle to set.
#
#  Usage:  sudo ./connector.sh
# =============================================================================
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LAN=192.168.44           # DS gets 192.168.44.10-100; AP is .1
TAP=nwc0
DNS=164.132.44.106       # Wiimmfi DNS (redirects *.nintendowifi.net); 8.8.8.8 is the fallback
VID=0411; PID=008b       # Nintendo Wi-Fi USB Connector (RT2570)

say(){ printf '\033[1;36m==> %s\033[0m\n' "$*"; }
warn(){ printf '\033[1;33m[!] %s\033[0m\n' "$*"; }
die(){ printf '\033[1;31m[x] %s\033[0m\n' "$*" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || die "run with sudo:  sudo ./connector.sh"

# --- 1. dependencies -------------------------------------------------------
say "Checking dependencies"
need_pkg=""
command -v gcc      >/dev/null || need_pkg+=" build-essential"
command -v dnsmasq  >/dev/null || need_pkg+=" dnsmasq"
command -v iptables >/dev/null || need_pkg+=" iptables"
[ -e /usr/include/libusb-1.0/libusb.h ] || need_pkg+=" libusb-1.0-0-dev"
if [ -n "$need_pkg" ]; then
  if command -v apt-get >/dev/null; then
    say "Installing:$need_pkg"
    DEBIAN_FRONTEND=noninteractive apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $need_pkg
  else
    die "Missing:$need_pkg . Install them with your package manager and re-run."
  fi
fi

# --- 2. locate/build the probe ---------------------------------------------
# Release tarballs ship a prebuilt ./nwcusb_probe. From a source clone, build it.
if [ ! -x "$HERE/nwcusb_probe" ]; then
  if [ -f "$HERE/build.sh" ]; then say "Building nwcusb_probe"; "$HERE/build.sh" || die "build failed"
  elif [ -f "$HERE/../src/probe/nwcusb_probe.c" ]; then
    say "Building nwcusb_probe from src/probe"
    ( cd "$HERE/../src/probe" && gcc -O2 -Wall -Wno-unused-function -o "$HERE/nwcusb_probe" nwcusb_probe.c -I. -I/usr/include/libusb-1.0 -lusb-1.0 ) || die "build failed"
  else die "nwcusb_probe not found. Use a release tarball, or build src/probe/nwcusb_probe.c first."
  fi
fi

# --- 3. find the dongle (any unit: USB 0411:008b) --------------------------
say "Locating the Nintendo Wi-Fi USB Connector (USB $VID:$PID)"
USBDEV=""
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/idVendor" ] || continue
  if [ "$(cat "$d/idVendor" 2>/dev/null)" = "$VID" ] && [ "$(cat "$d/idProduct" 2>/dev/null)" = "$PID" ]; then
    USBDEV="$(basename "$d")"; break
  fi
done
[ -n "$USBDEV" ] || die "No Nintendo Wi-Fi USB Connector found. Plug it in and re-run."
say "Found dongle at USB $USBDEV"

# --- 4. free it from the in-kernel rt2500usb driver (race-safe) ------------
# The kernel auto-binds rt2500usb; the probe needs the raw USB device. Unbind
# the interface and confirm it stays unbound before starting (a naive unbind
# races the kernel re-probe -> LIBUSB_ERROR_BUSY).
IFACE_DEV="${USBDEV}:1.0"
for try in 1 2 3 4 5; do
  if [ -e "/sys/bus/usb/drivers/rt2500usb/$IFACE_DEV" ]; then
    echo -n "$IFACE_DEV" > /sys/bus/usb/drivers/rt2500usb/unbind 2>/dev/null || true
    sleep 0.5
  fi
  [ -e "/sys/bus/usb/drivers/rt2500usb/$IFACE_DEV" ] || { say "Dongle freed from rt2500usb"; break; }
  [ "$try" = 5 ] && warn "rt2500usb keeps re-binding; the probe may report BUSY."
done
# also stop NetworkManager from grabbing it (best-effort)
command -v nmcli >/dev/null && nmcli dev set "$(basename "$USBDEV")" managed no 2>/dev/null || true

# --- 5. uplink + NAT + TAP -------------------------------------------------
UPLINK="$(ip -o route get 8.8.8.8 2>/dev/null | grep -oP 'dev \K\S+' | head -1)"
[ -n "$UPLINK" ] || die "No internet uplink (default route) found."
say "Uplink: $UPLINK   TAP: $TAP ($LAN.1/24)"
sysctl -wq net.ipv4.ip_forward=1
iptables -t nat -C POSTROUTING -o "$UPLINK" -j MASQUERADE 2>/dev/null || iptables -t nat -A POSTROUTING -o "$UPLINK" -j MASQUERADE
iptables -C FORWARD -i "$TAP" -o "$UPLINK" -j ACCEPT 2>/dev/null || iptables -A FORWARD -i "$TAP" -o "$UPLINK" -j ACCEPT
iptables -C FORWARD -i "$UPLINK" -o "$TAP" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || iptables -A FORWARD -i "$UPLINK" -o "$TAP" -m state --state RELATED,ESTABLISHED -j ACCEPT
ip tuntap add dev "$TAP" mode tap 2>/dev/null || true
ip addr replace "$LAN.1/24" dev "$TAP"
ip link set "$TAP" up

# --- 6. DHCP + Wiimmfi DNS (dnsmasq, scoped to the TAP) --------------------
DNSMASQ_CONF="$(mktemp /tmp/nwc-dnsmasq.XXXX.conf)"
cat > "$DNSMASQ_CONF" <<CONF
port=0
interface=$TAP
bind-dynamic
dhcp-range=$LAN.10,$LAN.100,255.255.255.0,12h
dhcp-option=3,$LAN.1
dhcp-option=6,$DNS,8.8.8.8
dhcp-authoritative
CONF
pkill -f "dnsmasq.*$TAP" 2>/dev/null || true; sleep 1
dnsmasq --conf-file="$DNSMASQ_CONF" --pid-file=/run/nwc-dnsmasq.pid

# --- 7. cleanup on exit ----------------------------------------------------
STOP=0
cleanup(){
  STOP=1
  say "Stopping + cleaning up"
  pkill -f "$HERE/nwcusb_probe" 2>/dev/null || true
  [ -f /run/nwc-dnsmasq.pid ] && kill "$(cat /run/nwc-dnsmasq.pid)" 2>/dev/null || true
  iptables -t nat -D POSTROUTING -o "$UPLINK" -j MASQUERADE 2>/dev/null || true
  ip link set "$TAP" down 2>/dev/null || true
  ip tuntap del dev "$TAP" mode tap 2>/dev/null || true
  rm -f "$DNSMASQ_CONF"
}
trap cleanup EXIT INT TERM

# --- 8. run the connector (foreground; auto-restart if it exits) -----------
say "READY. On the DS: Nintendo WFC Setup -> Connect to your Nintendo Wi-Fi USB Connector -> test."
say "Then play Mario Kart DS / Metroid Prime Hunters online. Ctrl+C to stop."
while [ "$STOP" = 0 ]; do
  NWC_DATAPATH=1 NWC_CSR0=0x1ec2 NWC_PROBE_MINGAP_MS=300 "$HERE/nwcusb_probe" ap-loop 1 || true
  [ "$STOP" = 0 ] || break
  warn "probe exited - restarting in 3s (Ctrl+C to quit)"
  sleep 3
done
