which wget >/dev/null 2>&1 || { echo "installing wget..."; apt-get -y install wget >/dev/null 2>&1; }
which wget >/dev/null 2>&1 || { echo "no wget/curl available"; exit 1; }
mkdir -p /home/user/nwc/rt2500usb-src; cd /home/user/nwc/rt2500usb-src
BASE="https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/wireless/ralink/rt2x00"
for f in rt2500usb.h rt2500usb.c rt2x00usb.c rt2x00usb.h rt2x00queue.h rt2x00.h rt2x00dev.c; do
  wget -q "$BASE/$f" -O "$f" && echo "got $f ($(wc -c <$f)b)" || echo "FAIL $f"
done
echo "=== beacon endpoint / usb_endpoint / QID_BEACON refs ==="
grep -nE 'usb_endpoint|QID_BEACON|bcn_priv|guardian' rt2500usb.c 2>/dev/null | head -15
chown -R user:user /home/user/nwc/rt2500usb-src
