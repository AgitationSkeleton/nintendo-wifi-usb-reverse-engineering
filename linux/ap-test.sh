#!/usr/bin/env bash
# =============================================================================
#  EXPERIMENT 1 — "Does the RT2570 auto-ACK under a known-good driver?"
#
#  Runs the Nintendo Wi-Fi USB Connector dongle as a plain OPEN access point
#  using Linux's mainline rt2500usb driver + hostapd, then waits for a DS to
#  associate via WFC's "Search for an Access Point".
#
#  WHY THIS MATTERS
#    Our Windows reimplementation never gets the chip to emit the hardware SIFS
#    ACK for the DS's auth seq1, so the DS never advances (error 51303). If the
#    DS associates HERE, the silicon is fine and our register configuration is
#    the bug -- and we then have a fully open-source reference (rt2500usb) whose
#    register writes we can capture with usbmon and diff against ours.
#    If it fails here too, the problem is deeper (chip fault or a real driver
#    limitation) and the whole effort redirects.
#
#  NOTE: OPEN network on purpose -- no WEP. Open-system auth exercises the exact
#  same MAC-layer ACK requirement, and WEP has been stripped from modern hostapd.
#
#      sudo bash ap-test.sh [channel]      (default channel 1)
# =============================================================================
set -uo pipefail
CH="${1:-1}"
SSID="NWCTEST"
say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
warn(){ printf "\033[1;33m[!] %s\033[0m\n" "$*"; }
ok(){ printf "\033[1;32m[ok] %s\033[0m\n" "$*"; }
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

# ---------------------------------------------------------------- find dongle
say "Looking for the RT2570 dongle"
lsusb | grep -iE "0411:008b|148f:2570|ralink|nintendo" || warn "no obvious RT2570 in lsusb"
if ! lsmod | grep -q rt2500usb; then modprobe rt2500usb 2>/dev/null; fi

# Force-bind if the USB ID isn't in the driver's table (the Nintendo unit is a
# rebadge, so it may not be listed).
if ! ls /sys/bus/usb/drivers/rt2500usb/ 2>/dev/null | grep -q ':'; then
  warn "rt2500usb has no bound device; trying force-bind of 0411:008b"
  echo "0411 008b" > /sys/bus/usb/drivers/rt2500usb/new_id 2>/dev/null || true
  sleep 2
fi

# Identify the wireless interface belonging to rt2500usb
IF=""
for i in $(iw dev 2>/dev/null | awk '/Interface/{print $2}'); do
  drv=$(basename "$(readlink -f "/sys/class/net/$i/device/driver" 2>/dev/null)" 2>/dev/null)
  [ "$drv" = "rt2500usb" ] && IF="$i" && break
done
[ -n "$IF" ] || { warn "Could not find an rt2500usb interface."; echo "  iw dev output:"; iw dev; \
                  echo "  dmesg tail:"; dmesg | tail -20; exit 1; }
ok "dongle interface: $IF"

# ---------------------------------------------------------------- does it even do AP?
say "Checking whether this chip advertises AP mode"
PHY=$(iw dev "$IF" info | awk '/wiphy/{print "phy"$2}')
if iw phy "$PHY" info | grep -A20 "Supported interface modes" | grep -q "\* AP"; then
  ok "AP mode supported by $PHY"
else
  warn "AP mode NOT advertised by $PHY -- hostapd will likely fail."
  warn "That itself is a finding: rt2500usb may not support AP on this kernel."
fi

# ---------------------------------------------------------------- run hostapd
say "Starting an OPEN access point: ssid='$SSID' channel=$CH"
rfkill unblock all 2>/dev/null
nmcli device set "$IF" managed no 2>/dev/null || true   # keep NetworkManager off it
ip link set "$IF" down 2>/dev/null; ip link set "$IF" up 2>/dev/null

CONF=/tmp/nwc-hostapd.conf
cat > "$CONF" <<EOF
interface=$IF
driver=nl80211
ssid=$SSID
hw_mode=g
channel=$CH
auth_algs=1
wmm_enabled=0
ignore_broadcast_ssid=0
# 802.11b basic rates so a DS (1-2 Mbps DSSS) can join
supported_rates=10 20 55 110
basic_rates=10 20
EOF

cat <<EOF

  On the DS:  WFC Setup -> Connection 1 -> Search for an Access Point -> "$SSID"
              (no password -- it is an open network)

  WATCH THE OUTPUT BELOW:
    "authenticated"  then  "associated"  then  "AP-STA-CONNECTED"
        => THE DS ASSOCIATED. The RT2570 auto-ACK works under rt2500usb.
           => our Windows register config is the bug. HUGE result.
    nothing / repeated "authentication attempts" with no association
        => the chip is not acknowledging here either -> deeper problem.

  Ctrl-C to stop.

EOF
say "hostapd starting (verbose)"
hostapd -dd "$CONF" 2>&1 | tee /tmp/nwc-hostapd.log
