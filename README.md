# USBEth — USB Ethernet for ESP32-P4

Wired Ethernet on ESP32-P4 boards via any class-compliant USB dongle. Plug in a USB-to-Ethernet adapter, call `USBEth.begin()`, get an IP address.

Uses the CDC-ECM (Ethernet Control Model) USB class protocol — no vendor-specific drivers needed. Works in both **Arduino IDE** and **ESP-IDF**.

## The Easy Way

### ESP-IDF (any ESP32-P4 board)

No patching. No shims. Full sdkconfig control.

```bash
cd examples/basic
idf.py set-target esp32p4
idf.py build flash monitor
```

The `sdkconfig.defaults` enables everything you need: `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK`, lwIP tuning, PSRAM. Just build and flash.

### Arduino IDE (official Espressif ESP32 Board Package)

If your board uses the **official Espressif Arduino core** (not a vendor BSP), you have sdkconfig control. Enable the enum filter callback in your board's sdkconfig:

```
CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y
```

Then install the library:

1. Copy `arduino/USBEth/` to your `~/Arduino/libraries/` folder
2. Open File → Examples → USBEth → BasicEthernet
3. Compile and flash

For best throughput, also add the lwIP tuning flags (see `examples/basic/sdkconfig.defaults` for the full list).

### Minimal Sketch

```cpp
#include <USBEth.h>

void setup() {
    Serial.begin(115200);
    USBEth.begin();
    while (!USBEth.hasIP()) delay(500);
    Serial.println(USBEth.localIP());
}

void loop() {
    delay(10000);
}
```

That's it. If your board and BSP give you sdkconfig control, USB Ethernet just works.

---

## The Hard Way (M5Stack Tab5 / Vendor BSPs)

The M5Stack BSP ships pre-compiled libraries from an older IDF snapshot with `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` disabled. You can't change this from an Arduino sketch — the setting is baked into the BSP's `libusb.a`. Without it, the USB host stack silently ignores the enum filter callback, the dongle stays in vendor-specific mode, and CDC-ECM is unreachable.

The fix is a one-time BSP patch that replaces the relevant `.a` files, adds component headers, and updates the linker flags. This also swaps `liblwip.a` for a version with tuned TCP settings (without this, throughput is 0.18 Mbps instead of 6+ Mbps).

### Option A: Use Pre-Built Binaries (recommended)

Download the pre-built BSP patch from the [Releases](../../releases) page, then:

```bash
# Unzip the patch
unzip usb-eth-bsp-patch.zip

# Patch the BSP (auto-detects M5Stack BSP location)
bash install.sh

# Install the Arduino library
cp -r USBEth ~/Arduino/libraries/
```

Open Arduino IDE, select M5Stack Tab5 board, open File → Examples → USBEth → BasicEthernet, compile and flash. Done.

### Option B: Build Everything from Source

If you want to understand (or modify) what's going on, here's the full build-from-source process. This is what generated the pre-built binaries above.

**Step 1: Build the patched libraries** (requires Linux/WSL, ~30 min one-time)

```bash
# Clone and build the Arduino lib-builder
git clone https://github.com/espressif/esp32-arduino-lib-builder.git
cd esp32-arduino-lib-builder

# Add USB Ethernet + lwIP tuning to the P4 config
cat >> configs/defconfig.esp32p4 << 'EOF'
CONFIG_ESP32P4_REV_MIN_0=y
CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y
CONFIG_LWIP_TCP_WND_DEFAULT=65535
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535
CONFIG_LWIP_TCP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCP_WND_SCALE=y
CONFIG_LWIP_TCP_RCV_SCALE=3
CONFIG_LWIP_IRAM_OPTIMIZATION=y
EOF

# Add USB ETH shim component
mkdir -p components/usb_eth_deps/include
cat > components/usb_eth_deps/CMakeLists.txt << 'EOF'
idf_component_register(
    SRCS "usb_eth_deps.c"
    INCLUDE_DIRS "include"
    REQUIRES iot_usbh_ecm iot_usbh_cdc iot_eth
)
EOF
echo 'void __usb_eth_deps_keep(void) {}' > components/usb_eth_deps/usb_eth_deps.c
cat > components/usb_eth_deps/idf_component.yml << 'EOF'
dependencies:
  espressif/iot_usbh_ecm: "^0.2.1"
  espressif/iot_usbh_cdc: "^3.0.0"
  espressif/iot_eth: "*"
EOF

# Build
./build.sh -t esp32p4
```

**Step 2: Build the HAL shim** (same terminal)

```bash
cp arduino/hal_shim/usb_dwc_hal_shim.c .
M5_P4="<your-bsp-esp32p4-path>"  # see install.sh output for auto-detection

~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc -c \
    -march=rv32imafc_zicsr_zifencei -mabi=ilp32f -Os -DSOC_USB_OTG_SUPPORTED=1 \
    -I "$M5_P4/include" -I "$M5_P4/qio_qspi/include" \
    -I esp-idf/components/hal/include \
    -I esp-idf/components/hal/platform_port/include \
    -I esp-idf/components/hal/esp32p4/include \
    -I esp-idf/components/soc/esp32p4/include \
    -I esp-idf/components/soc/include \
    -I esp-idf/components/esp_common/include \
    -I esp-idf/components/esp_hw_support/include \
    -I esp-idf/components/log/include \
    -I esp-idf/components/heap/include \
    usb_dwc_hal_shim.c -o usb_dwc_hal_shim.o

~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-ar rcs libusb_dwc_hal_shim.a usb_dwc_hal_shim.o
```

**Step 3: Collect and install**

```bash
# Collect libraries from lib-builder output
bash arduino/bsp-patch/collect_libs.sh .

# Patch the BSP (auto-detects location)
bash arduino/bsp-patch/install.sh

# Install the Arduino library
cp -r arduino/USBEth ~/Arduino/libraries/
```

**Step 4: Flash**

Open Arduino IDE, select M5Stack Tab5 board, open File → Examples → USBEth → BasicEthernet, compile and flash.

### Why Three Shims?

The BSP patch bridges version drift between the lib-builder's IDF (release/v5.4 HEAD) and the M5Stack BSP's older IDF snapshot. Three ABI mismatches had to be fixed:

1. **esp_log shim** — Newer IDF uses `esp_log()`, BSP has `esp_log_writev()`. A weak function bridge in USBEth.cpp routes one to the other.
2. **HAL FIFO shim** — Two functions (`usb_dwc_hal_fifo_config_is_valid`, `usb_dwc_hal_set_fifo_config`) were added between IDF snapshots. Compiled as `libusb_dwc_hal_shim.a` with `-mabi=ilp32f` (RISC-V single-float ABI — get this wrong and the linker explodes).
3. **Struct size shim** — `usb_host_config_t` grew from 12 to 28 bytes. New fields: `peripheral_map`, `root_port_unpowered`, `fifo_settings_custom`. Without the fix, `usb_host_install()` reads stack garbage and returns `ESP_ERR_INVALID_ARG`. USBEth.cpp defines the real 28-byte layout and casts.

These shims are harmless on a properly built IDF — the real functions and struct sizes already match, so the shims are never reached.

---

## Supported Dongles

Any CDC-ECM compliant dongle should work. Dongles not in the built-in table default to USB configuration 1.

| Dongle | VID:PID | ECM Config | Notes |
|--------|---------|------------|-------|
| ASIX AX88179B | 0B95:1790 | 3 | **Recommended.** Robust ECM, scales well. |
| Realtek RTL8152 | 0BDA:8152 | 2 | OK for basic use. Slower under parallel load. |

**Shopping tip:** "Supports Nintendo Switch" usually means class-compliant CDC-ECM.

## Performance

Measured throughput downloading 10 MB from Cloudflare CDN. Arduino and IDF builds achieve the same performance when using the tuned lwIP settings.

| Test | ASIX (IDF) | ASIX (Arduino) | RTL8152 (IDF) | RTL8152 (Arduino) |
|------|-----------|----------------|---------------|-------------------|
| Single stream | 6.78 Mbps | 6.43 Mbps | 6.29 Mbps | 6.38 Mbps |
| 4× parallel | 13.00 Mbps | 14.70 Mbps | 2.99 Mbps | 6.54 Mbps |

CDC-ECM theoretical ceiling over USB 2.0 HS is ~20-25 Mbps. Achieving 14.70 Mbps is ~65% of theoretical — good for a class-compliant driver with no vendor shortcuts.

## API

```cpp
#include <USBEth.h>

// Lifecycle
USBEth.begin()                     // Init with Tab5 defaults
USBEth.begin(config)               // Init with custom config
USBEth.end()                       // Stop

// Status
USBEth.connected()                 // Dongle present + ECM link up?
USBEth.hasIP()                     // DHCP lease obtained?

// Network info (valid after hasIP() == true)
USBEth.localIP()                   // IPAddress
USBEth.subnetMask()                // IPAddress
USBEth.gatewayIP()                 // IPAddress
USBEth.dnsIP(idx)                  // IPAddress (0=main, 1=backup)
USBEth.macAddress()                // String "AA:BB:CC:DD:EE:FF"
USBEth.linkSpeed()                 // Mbps (1000 for gigabit dongles)
USBEth.netif()                     // Raw esp_netif_t* for advanced use

// Configuration (call before begin)
USBEth.setHostname("name")         // mDNS hostname
USBEth.config(ip, gw, mask)        // Static IP (skip DHCP)
USBEth.onEvent(callback, arg)      // Event notifications
```

### Events

```cpp
void onEthEvent(usb_eth_event_t event, void *arg) {
    switch (event) {
        case USB_ETH_EVENT_GOT_IP:       // DHCP lease obtained
        case USB_ETH_EVENT_LOST_IP:      // IP lost
        case USB_ETH_EVENT_CONNECTED:    // Dongle enumerated + link up
        case USB_ETH_EVENT_DISCONNECTED: // Dongle removed or link down
    }
}
USBEth.onEvent(onEthEvent);
```

### Adding Custom Dongles

```cpp
static const usb_eth_dongle_t my_dongles[] = {
    { 0x1234, 0x5678, 2, "My Dongle (ECM in cfg 2)" },
};
usb_eth_config_t cfg = USB_ETH_CONFIG_TAB5();
cfg.dongles = my_dongles;
cfg.dongle_count = 1;
USBEth.begin(cfg);
```

## Tab5 VBUS Note

The Tab5's USB-A port (J10) VBUS power is gated by an I2C IO expander. `USBEth.begin()` enables it automatically. If the dongle doesn't enumerate, try a powered USB hub or solder a wire from a 5V pad to J10 VCC.

## Requirements

- **ESP-IDF v5.4.x** (for IDF builds)
- **ESP32-P4 board** with USB-A host port
- **CDC-ECM USB Ethernet dongle**
- **M5Stack BSP + BSP patch** (for Tab5 Arduino IDE builds only)

## Repository Structure

```
arduino/
  USBEth/                    # Arduino library (copy to ~/Arduino/libraries/)
    src/USBEth.{h,cpp}       # Library source
    examples/                # BasicEthernet, Benchmark
    library.properties
  bsp-patch/                 # BSP patch tools (Tab5 only)
    install.sh               # One-click BSP patcher
    collect_libs.sh          # Gather libs from lib-builder
    build_shim.sh            # Compile HAL shim
  hal_shim/                  # HAL shim source
    usb_dwc_hal_shim.c
  lib-builder-patch/         # Lib-builder config patches
components/usb_eth/          # ESP-IDF component (for IDF builds)
examples/basic/              # IDF example project
docs/                        # Blog post + technical diary
```

## License

MIT
