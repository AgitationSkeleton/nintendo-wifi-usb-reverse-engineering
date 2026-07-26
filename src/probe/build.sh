#!/usr/bin/env bash
# Build nwcusb_probe for Linux. Needs: gcc, libusb-1.0 dev headers.
#   sudo apt-get install build-essential libusb-1.0-0-dev
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
CFLAGS="$(pkg-config --cflags libusb-1.0 2>/dev/null || echo -I/usr/include/libusb-1.0)"
LIBS="$(pkg-config --libs libusb-1.0 2>/dev/null || echo -lusb-1.0)"
gcc -O2 -Wall -o nwcusb_probe nwcusb_probe.c $CFLAGS $LIBS -lpthread
echo "built: $HERE/nwcusb_probe"
