#!/bin/bash
# build_shim.sh — Compile the HAL shim and install into M5Stack BSP
#
# Run from WSL or Git Bash. Assumes:
#   - esp32-arduino-lib-builder is at ~/esp32-arduino-lib-builder (for IDF headers)
#   - M5Stack BSP installed at the standard Arduino15 path
#
# Usage: bash build_shim.sh

set -e

# ── Paths (adjust if yours differ) ──────────────────────────────
LIBBUILDER="$HOME/esp32-arduino-lib-builder"
IDF_SRC="$LIBBUILDER/esp-idf"
TOOLCHAIN="$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin"
GCC="$TOOLCHAIN/riscv32-esp-elf-gcc"
AR="$TOOLCHAIN/riscv32-esp-elf-ar"

# M5Stack BSP esp32p4 libs
M5_BSP_BASE="/mnt/c/Users/$USER/AppData/Local/Arduino15/packages/m5stack/tools/esp32-arduino-libs"
M5_P4=$(find "$M5_BSP_BASE" -maxdepth 2 -type d -name "esp32p4" 2>/dev/null | head -1)

if [ -z "$M5_P4" ]; then
    echo "ERROR: Cannot find M5Stack BSP esp32p4 directory"
    exit 1
fi

echo "IDF source:  $IDF_SRC"
echo "M5Stack BSP: $M5_P4"
echo "Toolchain:   $GCC"
echo

# ── Compile ──────────────────────────────────────────────────────
echo "Compiling usb_dwc_hal_shim.c ..."
$GCC -c \
    -march=rv32imafc_zicsr_zifencei -mabi=ilp32f \
    -Os \
    -DSOC_USB_OTG_SUPPORTED=1 \
    -I "$M5_P4/include" \
    -I "$IDF_SRC/components/hal/include" \
    -I "$IDF_SRC/components/hal/platform_port/include" \
    -I "$IDF_SRC/components/hal/esp32p4/include" \
    -I "$IDF_SRC/components/soc/esp32p4/include" \
    -I "$IDF_SRC/components/soc/include" \
    -I "$IDF_SRC/components/esp_common/include" \
    -I "$IDF_SRC/components/esp_hw_support/include" \
    -I "$IDF_SRC/components/log/include" \
    -I "$IDF_SRC/components/heap/include" \
    usb_dwc_hal_shim.c -o usb_dwc_hal_shim.o

echo "Creating libusb_dwc_hal_shim.a ..."
$AR rcs libusb_dwc_hal_shim.a usb_dwc_hal_shim.o

# ── Verify — should show ONLY our 2 functions ───────────────────
echo
echo "Symbols in shim library:"
$TOOLCHAIN/riscv32-esp-elf-nm libusb_dwc_hal_shim.a | grep " T "
echo

# ── Install ──────────────────────────────────────────────────────
echo "Installing to $M5_P4/lib/ ..."
cp libusb_dwc_hal_shim.a "$M5_P4/lib/"
echo "Done!"
echo
echo "Verify ld_libs contains: -lusb_dwc_hal_shim"
grep -o "usb_dwc_hal_shim" "$M5_P4/flags/ld_libs" && echo "  ✓ already in ld_libs" || echo "  ✗ NOT in ld_libs — add it!"
