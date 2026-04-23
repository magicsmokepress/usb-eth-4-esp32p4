#!/bin/bash
# collect_libs.sh — Gather all patched libraries + headers from lib-builder output
#
# Run this AFTER a successful lib-builder build:
#   cd ~/esp32-arduino-lib-builder
#   bash /path/to/collect_libs.sh
#
# Creates a self-contained bsp-patch/ directory that install.sh can apply.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH_DIR="$SCRIPT_DIR"
LIBBUILDER="${1:-.}"  # default: current directory

# Find lib-builder output
OUT=$(find "$LIBBUILDER/out/tools/esp32-arduino-libs" -maxdepth 2 -type d -name "esp32p4" 2>/dev/null | head -1)
if [ -z "$OUT" ]; then
    echo "ERROR: Cannot find lib-builder output for esp32p4"
    echo "Usage: $0 [path-to-esp32-arduino-lib-builder]"
    exit 1
fi
echo "Lib-builder output: $OUT"

# ── Libraries ────────────────────────────────────────────────
mkdir -p "$PATCH_DIR/lib"

# Core USB + ETH component libraries
for lib in libespressif__iot_usbh_ecm.a libespressif__iot_usbh_cdc.a \
           libespressif__iot_eth.a libusb_eth_deps.a libusb.a liblwip.a; do
    if [ -f "$OUT/lib/$lib" ]; then
        cp "$OUT/lib/$lib" "$PATCH_DIR/lib/"
        echo "  Copied: $lib"
    else
        echo "  WARNING: $lib not found in lib-builder output"
    fi
done

# HAL shim (built separately)
if [ -f "$LIBBUILDER/libusb_dwc_hal_shim.a" ]; then
    cp "$LIBBUILDER/libusb_dwc_hal_shim.a" "$PATCH_DIR/lib/"
    echo "  Copied: libusb_dwc_hal_shim.a"
elif [ -f "$PATCH_DIR/lib/libusb_dwc_hal_shim.a" ]; then
    echo "  Using existing: libusb_dwc_hal_shim.a"
else
    echo "  WARNING: libusb_dwc_hal_shim.a not found — build it with build_shim.sh first"
fi

# ── Headers ──────────────────────────────────────────────────
mkdir -p "$PATCH_DIR/include"

for comp in espressif__iot_eth espressif__iot_usbh_cdc espressif__iot_usbh_ecm usb_eth_deps; do
    if [ -d "$OUT/include/$comp" ]; then
        cp -r "$OUT/include/$comp" "$PATCH_DIR/include/"
        echo "  Copied headers: $comp/"
    fi
done

# ── Done ─────────────────────────────────────────────────────
echo
echo "Patch directory ready: $PATCH_DIR"
echo "  lib/     — $(ls "$PATCH_DIR/lib/" | wc -l) libraries"
echo "  include/ — $(ls "$PATCH_DIR/include/" | wc -l) component header dirs"
echo
echo "Next: run install.sh to apply to your M5Stack BSP"
