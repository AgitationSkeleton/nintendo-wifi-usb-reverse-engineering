#!/usr/bin/env bash
# =============================================================================
#  EXPERIMENT 2 — "Does an ACK appear on the air after the DS's auth seq1?"
#
#  The whole project is blocked on one invisible event: the RT2570 is supposed to
#  emit an 802.11 ACK ~10us (one SIFS) after receiving the DS's authentication
#  frame. That ACK is generated inside the chip and NEVER crosses the USB bus, so
#  no USB capture can see it. Only an over-the-air capture can.
#
#  Two outcomes, and they need OPPOSITE fixes:
#     ACK absent  -> the auto-responder is disarmed  -> keep hunting chip state
#     ACK present -> the DS can't decode it (rate/preamble/timing) -> different bug
#
#  *** METHODOLOGY -- READ THIS ***
#  "I saw no ACK" is only meaningful if we PROVE this rig can capture ACKs at all.
#  ACKs are 14 bytes and easy to miss. So this script runs a CONTROL first: it
#  counts ACK frames from ordinary nearby traffic. If the control sees zero ACKs,
#  the capture is not trustworthy and a negative result means nothing.
#
#      sudo bash sniff.sh <AP_MAC> [channel] [seconds]
#      e.g. sudo bash sniff.sh 00:11:22:33:44:55 1 180
#
#  AP_MAC = the connector dongle's BSSID (printed by the Windows probe at startup).
# =============================================================================
set -uo pipefail
APMAC="${1:-}"; CH="${2:-1}"; SECS="${3:-180}"
OUT="/tmp/nwc-capture-$(date +%H%M%S).pcapng"
say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
warn(){ printf "\033[1;33m[!] %s\033[0m\n" "$*"; }
ok(){ printf "\033[1;32m[ok] %s\033[0m\n" "$*"; }
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -n "$APMAC" ] || { echo "usage: sudo bash sniff.sh <AP_MAC> [channel] [seconds]"; exit 1; }
APLC=$(echo "$APMAC" | tr 'A-Z' 'a-z')

# ---------------------------------------------------------------- verify adapter
say "Verifying the sniffer is a genuine Atheros AR9271"
if lsusb | grep -qi "0cf3:9271"; then
  ok "AR9271 present: $(lsusb | grep -i 0cf3:9271)"
else
  warn "No 0cf3:9271 found. If your adapter shows a Realtek ID (0bda:...) it is a"
  warn "relabelled counterfeit -- return it; ath9k_htc is what we need."
  lsusb; exit 1
fi
dmesg | grep -i "ath9k_htc" | tail -3
lsmod | grep -q ath9k_htc || modprobe ath9k_htc 2>/dev/null

# ---------------------------------------------------------------- monitor mode
MON=""
for i in $(iw dev 2>/dev/null | awk '/Interface/{print $2}'); do
  drv=$(basename "$(readlink -f "/sys/class/net/$i/device/driver" 2>/dev/null)" 2>/dev/null)
  [ "$drv" = "ath9k_htc" ] && MON="$i" && break
done
[ -n "$MON" ] || { warn "no ath9k_htc interface found"; iw dev; exit 1; }

say "Putting $MON into monitor mode on channel $CH"
airmon-ng check kill >/dev/null 2>&1
ip link set "$MON" down
iw dev "$MON" set type monitor 2>/dev/null || iw dev "$MON" set monitor control
ip link set "$MON" up
iw dev "$MON" set channel "$CH" || { warn "could not set channel $CH"; exit 1; }
ok "$MON monitoring channel $CH"

# ---------------------------------------------------------------- CONTROL
say "CONTROL (10s): can this rig capture ACK frames at all?"
CTRL=/tmp/nwc-control.pcapng
timeout 12 tshark -i "$MON" -w "$CTRL" -q >/dev/null 2>&1
CTRL_ACKS=$(tshark -r "$CTRL" -Y 'wlan.fc.type_subtype == 0x1d' 2>/dev/null | wc -l)
CTRL_ALL=$(tshark -r "$CTRL" 2>/dev/null | wc -l)
echo "    frames: $CTRL_ALL    ACK frames: $CTRL_ACKS"
if [ "$CTRL_ACKS" -gt 0 ]; then
  ok "Control PASSED -- this rig does capture ACKs, so a negative result will be meaningful."
else
  warn "Control FAILED -- zero ACKs captured from ambient traffic."
  warn "A 'no ACK' result would be INCONCLUSIVE. Move closer to the AP/DS, confirm the"
  warn "channel is right, and re-run. (Some quiet channels simply have no traffic.)"
fi

# ---------------------------------------------------------------- main capture
say "MAIN CAPTURE (${SECS}s) -- start the DS connection attempt NOW"
cat <<EOF

    AP (dongle) BSSID : $APMAC
    channel           : $CH
    writing           : $OUT

    Trigger the DS now: WFC Setup -> connect. Let it run until it errors.

EOF
timeout "$SECS" tshark -i "$MON" -w "$OUT" -q >/dev/null 2>&1
ok "capture finished: $OUT"

# ---------------------------------------------------------------- analysis
say "ANALYSIS"
echo "--- authentication / association frames involving the AP ---"
tshark -r "$OUT" -Y "wlan.addr == $APLC && (wlan.fc.type_subtype==0x0b || wlan.fc.type_subtype==0x00 || wlan.fc.type_subtype==0x01)" \
  -T fields -e frame.number -e frame.time_relative -e wlan.fc.type_subtype \
  -e wlan.sa -e wlan.da -e wlan.fixed.auth_seq -e wlan.fc.retry 2>/dev/null | head -40

echo ""
echo "--- ACK frames addressed TO the AP (i.e. the DS acking us) ---"
ACK_TO_AP=$(tshark -r "$OUT" -Y "wlan.fc.type_subtype==0x1d && wlan.ra==$APLC" 2>/dev/null | wc -l)
echo "    count: $ACK_TO_AP"

echo ""
echo "--- *** THE KEY NUMBER *** ACK frames FROM the AP (the dongle acking the DS) ---"
# An ACK carries only a receiver address; an ACK *sent by the AP* is addressed to
# the DS. Find the DS MAC as the peer that authenticates to our BSSID, then count
# ACKs addressed to it.
DS=$(tshark -r "$OUT" -Y "wlan.fc.type_subtype==0x0b && wlan.da==$APLC" -T fields -e wlan.sa 2>/dev/null | sort -u | head -1)
if [ -n "$DS" ]; then
  echo "    DS detected as: $DS"
  ACK_TO_DS=$(tshark -r "$OUT" -Y "wlan.fc.type_subtype==0x1d && wlan.ra==$DS" 2>/dev/null | wc -l)
  AUTHS=$(tshark -r "$OUT" -Y "wlan.fc.type_subtype==0x0b && wlan.sa==$DS" 2>/dev/null | wc -l)
  echo "    DS auth frames sent      : $AUTHS"
  echo "    ACKs addressed to the DS : $ACK_TO_DS"
  echo ""
  if [ "$ACK_TO_DS" -gt 0 ]; then
    printf "\033[1;32m    VERDICT: ACKs ARE being transmitted to the DS.\033[0m\n"
    echo "    => the auto-responder IS firing; the DS is failing to accept/decode them."
    echo "    => investigate ACK rate / preamble / timing, NOT the arming registers."
  elif [ "$CTRL_ACKS" -gt 0 ]; then
    printf "\033[1;31m    VERDICT: NO ACKs to the DS, and the control proved we CAN see ACKs.\033[0m\n"
    echo "    => the auto-responder is genuinely disarmed. Keep hunting chip state."
  else
    printf "\033[1;33m    VERDICT: INCONCLUSIVE (control failed -- rig may not capture ACKs).\033[0m\n"
  fi
else
  warn "No DS auth frames to $APMAC captured. Was the AP running and the DS attempting?"
fi

echo ""
say "Full capture kept at $OUT  (copy it back for deeper analysis)"
