#!/usr/bin/env bash
# =============================================================================
#  NWC NUC — one-shot setup for the RT2570 experiments
#  Run this ONCE after booting the Ubuntu live session and getting online.
#
#      sudo bash setup.sh
#
#  It makes the machine remotely drivable (SSH + key + mDNS name) and installs
#  the wireless tooling for both experiments.
# =============================================================================
set -uo pipefail

HOSTNAME_WANTED="nuc"
SSH_USER="${SUDO_USER:-$(logname 2>/dev/null || echo ubuntu)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
warn(){ printf "\033[1;33m[!] %s\033[0m\n" "$*"; }
ok(){ printf "\033[1;32m[ok] %s\033[0m\n" "$*"; }

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo:  sudo bash setup.sh"; exit 1; }

# ---------------------------------------------------------------- networking
say "Checking network"
if ! ping -c1 -W3 1.1.1.1 >/dev/null 2>&1; then
  warn "No internet yet."
  echo "    Plug in Ethernet, or connect Wi-Fi from the desktop's network menu,"
  echo "    or use:   nmcli device wifi connect \"YOUR_SSID\" password \"YOUR_PASSWORD\""
  echo "    Then re-run this script."
  exit 1
fi
ok "Online"

# ---------------------------------------------------------------- hostname/mDNS
say "Setting hostname to '$HOSTNAME_WANTED' (so it's reachable as ${HOSTNAME_WANTED}.local)"
hostnamectl set-hostname "$HOSTNAME_WANTED" 2>/dev/null || true
grep -q "$HOSTNAME_WANTED" /etc/hosts || echo "127.0.1.1 $HOSTNAME_WANTED" >> /etc/hosts

# ---------------------------------------------------------------- packages
say "Installing tooling (this is the slow part)"
export DEBIAN_FRONTEND=noninteractive
apt-get update -y >/dev/null 2>&1
# openssh-server: remote control | avahi: .local name | rest: the experiments
apt-get install -y --no-install-recommends \
    openssh-server avahi-daemon libnss-mdns \
    aircrack-ng iw wireless-tools tshark tcpdump hostapd \
    usbutils pciutils linux-firmware rfkill \
  >/dev/null 2>&1 && ok "packages installed" || warn "some packages failed (see: apt-get install ...)"

# tshark must be usable by non-root for captures
echo "wireshark-common wireshark-common/install-setuid boolean true" | debconf-set-selections 2>/dev/null || true
usermod -aG wireshark "$SSH_USER" 2>/dev/null || true

# ---------------------------------------------------------------- ssh + key
say "Enabling SSH with key auth"
install -d -m 700 "/home/$SSH_USER/.ssh" 2>/dev/null || install -d -m 700 "/root/.ssh"
HOME_SSH="/home/$SSH_USER/.ssh"; [ -d "$HOME_SSH" ] || HOME_SSH="/root/.ssh"
if [ -f "$HERE/authorized_key.pub" ]; then
  cat "$HERE/authorized_key.pub" >> "$HOME_SSH/authorized_keys"
  sort -u "$HOME_SSH/authorized_keys" -o "$HOME_SSH/authorized_keys"
  chmod 600 "$HOME_SSH/authorized_keys"
  chown -R "$SSH_USER":"$SSH_USER" "$HOME_SSH" 2>/dev/null || true
  ok "remote key installed for user '$SSH_USER'"
else
  warn "authorized_key.pub not found next to this script — key auth NOT set up"
fi
# passwordless sudo for the experiments (live session only; convenience)
echo "$SSH_USER ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/99-nuc
chmod 440 /etc/sudoers.d/99-nuc
systemctl enable --now ssh >/dev/null 2>&1 || systemctl enable --now sshd >/dev/null 2>&1
systemctl enable --now avahi-daemon >/dev/null 2>&1
ok "sshd running"

# ---------------------------------------------------------------- radios
say "Unblocking radios"
rfkill unblock all 2>/dev/null || true

# ---------------------------------------------------------------- report
IPS=$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]' | paste -sd' ')
say "READY"
cat <<EOF

  hostname : $HOSTNAME_WANTED   (try ${HOSTNAME_WANTED}.local first)
  user     : $SSH_USER
  IP(s)    : $IPS

  From the Windows machine, connect with:
      host\connect-nuc.ps1
  or directly:
      ssh -i host\nuc_key $SSH_USER@${HOSTNAME_WANTED}.local

EOF

# Best-effort: leave a status file on the SMB share if it happens to be reachable.
bash "$HERE/phone-home.sh" 2>/dev/null || true

# Quick hardware inventory so the operator can see what's present.
say "Hardware seen"
echo "--- USB ---"; lsusb 2>/dev/null | grep -iE "atheros|ralink|0cf3|148f|0411|nintendo" || echo "  (no RT2570/AR9271 detected yet — plug them in)"
echo "--- wireless interfaces ---"; iw dev 2>/dev/null | grep -E "Interface|type" || echo "  (none)"
