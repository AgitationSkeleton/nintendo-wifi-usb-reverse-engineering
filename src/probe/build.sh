#!/bin/sh
# Build the connector probe on Linux (libusb backend, non-KMDF).
# Requires: gcc, libusb-1.0-0-dev  (apt-get install build-essential libusb-1.0-0-dev)
set -e
cd "$(dirname "$0")"
gcc -O2 -Wall -Wno-unused-function -o nwcusb_probe nwcusb_probe.c \
    -I. -I/usr/include/libusb-1.0 -lusb-1.0
echo "built: $(stat -c%s nwcusb_probe) bytes"
echo
echo "Run (RT2570 must be unbound from rt2500usb first):"
echo "  DEV=\$(basename \$(readlink -f /sys/class/net/wlxXXXX/device))"
echo "  echo -n \$DEV | sudo tee /sys/bus/usb/drivers/rt2500usb/unbind"
echo "  sudo ./nwcusb_probe ap-loop 1"
