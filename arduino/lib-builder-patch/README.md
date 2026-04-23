# Building a Custom Arduino Core for USB Ethernet on ESP32-P4

The stock Arduino-ESP32 board package has two blockers for USB Ethernet:

1. `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` is disabled (can't select ECM configuration)
2. Default target is P4 rev v3.0+ (Tab5 is v1.3 eco2 — won't boot)

This guide rebuilds the Arduino core libraries with both fixes, plus the USB ETH
components baked in. After this, USBEth works as a normal Arduino library.

## Prerequisites

- **Linux or WSL** (the lib-builder is bash-based)
- **ESP-IDF v5.4.x** installed and sourced (`source export.sh`)
- ~10 GB disk space, ~30 min build time
- Git, Python 3.8+

## Steps

### 1. Clone the lib-builder

```bash
git clone https://github.com/espressif/esp32-arduino-lib-builder
cd esp32-arduino-lib-builder
```

### 2. Patch defconfig.esp32p4

Edit `configs/defconfig.esp32p4`. At the top, replace:

```
# CONFIG_ESP32P4_SELECTS_REV_LESS_V3 is not set
```

with:

```
# USBEth: Support ESP32-P4 eco2 (Tab5 chip rev v1.3)
CONFIG_ESP32P4_REV_MIN_0=y

# USBEth: Enable USB enumeration filter (CRITICAL for CDC-ECM config selection)
CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y
```

Or apply the patch file:

```bash
git apply /path/to/defconfig.esp32p4.patch
```

### 3. Add USB ETH component dependencies

The lib-builder's project needs the iot_usbh_ecm/cdc/eth managed components.
Edit (or create) `main/idf_component.yml` and add under `dependencies:`:

```yaml
  espressif/iot_usbh_ecm: "^0.2.1"
  espressif/iot_usbh_cdc: "^3.0.0"
  espressif/iot_eth: "*"
```

### 4. Build for ESP32-P4

```bash
./build.sh -t esp32p4
```

This compiles the entire Arduino core + IDF + USB ETH components into static
libraries. First build takes ~30 minutes. Subsequent builds are faster.

If it succeeds you'll see: `Successfully created esp32p4 image.`

### 5. Install into Arduino IDE

Copy the built libraries to your Arduino installation:

```bash
# Find your Arduino sketchbook location:
# Arduino IDE → File → Preferences → Sketchbook location

./build.sh -c ~/Arduino/hardware/espressif/esp32
```

Or manually copy the output from `out/` to the appropriate directory.

Restart Arduino IDE 2.x. Select **Tools → Board → ESP32P4 Dev Module**.

### 6. Install USBEth library

Copy the `USBEth/` folder to your Arduino libraries directory:

```bash
cp -r /path/to/USBEth ~/Arduino/libraries/
```

### 7. Flash a sketch

Open **File → Examples → USBEth → BasicEthernet**.

Select:
- Board: ESP32P4 Dev Module
- USB CDC On Boot: Enabled (for Serial output via USB JTAG)
- Partition Scheme: "Huge APP" or custom (binary needs >1 MB)
- Flash Size: 16 MB

Click Upload.

## What Changed (2 lines)

The entire custom build exists because of two missing Kconfig options in the
stock board package:

| Option | Default | Our value | Why |
|--------|---------|-----------|-----|
| `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` | disabled | `y` | Select ECM config on multi-config dongles |
| `CONFIG_ESP32P4_REV_MIN_0` | not set | `y` | Support Tab5 eco2 silicon (rev v1.3) |

That's it. Two lines in defconfig. Everything else in the Arduino core is untouched.

## When This Becomes Unnecessary

If Espressif merges [arduino-esp32#11290](https://github.com/espressif/arduino-esp32/issues/11290)
(enable enum filter by default) and adds P4 eco2 support to the stock board
package, this entire custom build process goes away. USBEth becomes a
drop-in Arduino library with zero prerequisites.
