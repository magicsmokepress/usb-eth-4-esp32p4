#!/bin/bash
# install.sh — Patch M5Stack BSP for USB Ethernet support
#
# This script modifies the M5Stack Tab5 Arduino BSP to add:
#   - USB Ethernet component libraries (iot_usbh_ecm, iot_usbh_cdc, iot_eth)
#   - Replacement libusb.a with enum_filter_cb support
#   - Replacement liblwip.a with tuned TCP settings (64KB windows, scaling)
#   - HAL shim for DWC FIFO functions
#   - Component headers
#   - Linker flags and include paths
#   - sdkconfig patches
#
# Usage:
#   bash install.sh                          # auto-detect BSP location
#   bash install.sh /path/to/esp32p4/dir     # explicit BSP path
#
# To undo: restore from the .bak files this script creates.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Find BSP esp32p4 directory ───────────────────────────────

find_bsp() {
    local search_paths=(
        # WSL paths
        "/mnt/c/Users/*/AppData/Local/Arduino15/packages/m5stack/tools/esp32-arduino-libs/*/esp32p4"
        # macOS
        "$HOME/Library/Arduino15/packages/m5stack/tools/esp32-arduino-libs/*/esp32p4"
        # Linux
        "$HOME/.arduino15/packages/m5stack/tools/esp32-arduino-libs/*/esp32p4"
    )
    for pattern in "${search_paths[@]}"; do
        local found
        found=$(ls -d $pattern 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            echo "$found"
            return 0
        fi
    done
    return 1
}

if [ -n "$1" ]; then
    BSP_P4="$1"
else
    BSP_P4=$(find_bsp) || true
fi

if [ -z "$BSP_P4" ] || [ ! -d "$BSP_P4" ]; then
    echo "ERROR: Cannot find M5Stack BSP esp32p4 directory"
    echo "Usage: $0 [/path/to/esp32p4]"
    echo
    echo "Expected location (WSL):"
    echo "  /mnt/c/Users/<you>/AppData/Local/Arduino15/packages/m5stack/tools/esp32-arduino-libs/<version>/esp32p4"
    exit 1
fi

echo "╔══════════════════════════════════════════════════╗"
echo "║  USBEth BSP Patch Installer                      ║"
echo "╚══════════════════════════════════════════════════╝"
echo
echo "BSP target: $BSP_P4"
echo

# ── Verify patch directory has required files ────────────────

check_file() {
    if [ ! -f "$SCRIPT_DIR/$1" ]; then
        echo "ERROR: Missing $1 — run collect_libs.sh first"
        exit 1
    fi
}

check_file "lib/libusb.a"
check_file "lib/liblwip.a"
check_file "lib/libespressif__iot_usbh_ecm.a"
check_file "lib/libespressif__iot_usbh_cdc.a"
check_file "lib/libespressif__iot_eth.a"
check_file "lib/libusb_dwc_hal_shim.a"

echo "All required files present."
echo

# ── Step 1: Backup originals ────────────────────────────────

backup() {
    if [ -f "$1" ] && [ ! -f "$1.bak" ]; then
        cp "$1" "$1.bak"
        echo "  Backed up: $(basename "$1")"
    fi
}

echo "Step 1: Backing up original files..."
backup "$BSP_P4/lib/libusb.a"
backup "$BSP_P4/lib/liblwip.a"
backup "$BSP_P4/flags/ld_libs"
backup "$BSP_P4/flags/includes"
backup "$BSP_P4/sdkconfig"
echo

# ── Step 2: Install libraries ───────────────────────────────

echo "Step 2: Installing libraries..."
for lib in "$SCRIPT_DIR"/lib/*.a; do
    cp "$lib" "$BSP_P4/lib/"
    echo "  Installed: $(basename "$lib")"
done
echo

# ── Step 3: Install headers ─────────────────────────────────

echo "Step 3: Installing headers..."
for dir in "$SCRIPT_DIR"/include/*/; do
    dirname=$(basename "$dir")
    cp -r "$dir" "$BSP_P4/include/"
    echo "  Installed: $dirname/"
done
echo

# ── Step 4: Patch include flags ──────────────────────────────

echo "Step 4: Patching include flags..."
INCLUDES_FILE="$BSP_P4/flags/includes"

INCLUDE_ENTRIES=(
    "-iwithprefixbefore/espressif__iot_eth"
    "-iwithprefixbefore/espressif__iot_usbh_cdc"
    "-iwithprefixbefore/espressif__iot_usbh_ecm"
    "-iwithprefixbefore/usb_eth_deps"
)

for entry in "${INCLUDE_ENTRIES[@]}"; do
    if ! grep -qF "$entry" "$INCLUDES_FILE" 2>/dev/null; then
        echo " $entry" >> "$INCLUDES_FILE"
        echo "  Added: $entry"
    else
        echo "  Already present: $entry"
    fi
done
echo

# ── Step 5: Patch linker flags ───────────────────────────────

echo "Step 5: Patching linker flags..."
LD_LIBS_FILE="$BSP_P4/flags/ld_libs"

LD_ENTRIES=(
    "-lespressif__iot_usbh_ecm"
    "-lespressif__iot_usbh_cdc"
    "-lespressif__iot_eth"
    "-lusb_eth_deps"
    "-lusb_dwc_hal_shim"
)

for entry in "${LD_ENTRIES[@]}"; do
    if ! grep -qF "$entry" "$LD_LIBS_FILE" 2>/dev/null; then
        # Append after the last line
        sed -i "$ s/$/ $entry/" "$LD_LIBS_FILE"
        echo "  Added: $entry"
    else
        echo "  Already present: $entry"
    fi
done
echo

# ── Step 6: Patch sdkconfig ─────────────────────────────────

echo "Step 6: Patching sdkconfig..."
SDKCONFIG="$BSP_P4/sdkconfig"

patch_config() {
    local key="$1"
    local value="$2"
    if grep -q "^$key=" "$SDKCONFIG" 2>/dev/null; then
        if grep -q "^$key=$value" "$SDKCONFIG"; then
            echo "  Already set: $key=$value"
        else
            sed -i "s|^$key=.*|$key=$value|" "$SDKCONFIG"
            echo "  Updated: $key=$value"
        fi
    elif grep -q "^# $key is not set" "$SDKCONFIG" 2>/dev/null; then
        sed -i "s|^# $key is not set|$key=$value|" "$SDKCONFIG"
        echo "  Enabled: $key=$value"
    else
        echo "$key=$value" >> "$SDKCONFIG"
        echo "  Added: $key=$value"
    fi
}

patch_config "CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK" "y"
patch_config "CONFIG_LWIP_TCP_WND_DEFAULT" "65535"
patch_config "CONFIG_LWIP_TCP_SND_BUF_DEFAULT" "65535"
patch_config "CONFIG_LWIP_TCP_RECVMBOX_SIZE" "32"
patch_config "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE" "32"
patch_config "CONFIG_LWIP_TCP_WND_SCALE" "y"
patch_config "CONFIG_LWIP_TCP_RCV_SCALE" "3"
patch_config "CONFIG_LWIP_IRAM_OPTIMIZATION" "y"
echo

# ── Done ─────────────────────────────────────────────────────

echo "╔══════════════════════════════════════════════════╗"
echo "║  BSP patch complete!                              ║"
echo "╚══════════════════════════════════════════════════╝"
echo
echo "Next steps:"
echo "  1. Copy USBEth library to your Arduino libraries folder:"
echo "     cp -r USBEth/ ~/Arduino/libraries/"
echo "  2. Open Arduino IDE → File → Examples → USBEth → BasicEthernet"
echo "  3. Select board: M5Stack Tab5"
echo "  4. Compile + flash"
echo
echo "To undo this patch, restore the .bak files in:"
echo "  $BSP_P4/lib/"
echo "  $BSP_P4/flags/"
echo "  $BSP_P4/sdkconfig"
