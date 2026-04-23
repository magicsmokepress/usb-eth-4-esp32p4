# USB Ethernet on the ESP32-P4: A Class-Compliant Approach

## How a bad recommendation, a $7 dongle, and a lot of USB descriptor parsing led to 13 Mbps on the M5Stack Tab5

---

The M5Stack Tab5 is one of the most capable ESP32-P4 development boards available in early 2026. It has a 7-inch touchscreen, 32 MB of PSRAM, a MIPI-CSI camera port, and — critically for this story — a USB-A host port on the side. That port is intended for peripherals like keyboards and flash drives, but it's connected to the P4's dedicated 480 Mbps High-Speed USB OTG PHY. Which means, in theory, it can drive a USB Ethernet dongle.

In practice, getting from "in theory" to "13 Mbps over CDC-ECM" required solving five distinct problems that nobody had documented before. This is the story of that journey.

---

## The Wrong Dongle

This project started with bad advice. In an earlier conversation, an AI assistant recommended the Realtek RTL8152 as the target chip for a USB Ethernet library on the ESP32-P4. The user bought one. The recommendation was wrong — not because the RTL8152 is a bad chip, but because the right architectural choice for an independent library is to target *class-compliant USB protocols* (CDC-ECM and CDC-NCM), not vendor-specific ones.

Class-compliant protocols are defined by public USB-IF specifications. They work across vendors and chip generations. They need no reverse engineering, carry no GPL clean-room risk, and produce a library that survives any single vendor going hostile. A CDC-ECM driver doesn't care whether the silicon behind the USB connector says ASIX, Realtek, or Apple — if the device advertises `bInterfaceClass=0x02, bInterfaceSubClass=0x06`, it speaks ECM, and one driver handles them all.

The alternative — porting Realtek's vendor-specific protocol from the Linux `r8152.c` driver — would have taken weeks, produced a single-vendor result, and required careful GPL clean-room discipline to avoid contaminating an open-source library. For a development tool project, that's the wrong trade.

So the plan was revised: target class-compliant protocols first, with an ASIX AX88179B dongle (bought for $15 based on the "Nintendo Switch compatible" heuristic — if it works on a Switch, it must be class-compliant, because the Switch has no user-installable driver path). The RTL8152 would be tested too, but only if it happened to expose a CDC-ECM interface alongside its vendor one.

---

## Problem 1: The Invisible Power Gate

Before any software could run, there was a hardware mystery to solve. The Tab5's USB-A port (J10) doesn't supply power by default. VBUS is gated by a MT9700 load switch whose enable pin (`USB5V_EN`) is wired to pin P3 on a PI4IOE5V6408 I²C GPIO expander at address 0x44.

The PI4IOE5V6408 boots with all pins as high-impedance inputs. `M5.begin()` doesn't enable USB power — it enables WiFi, display, touch, and camera resets, but leaves `USB5V_EN` off. You have to explicitly drive U7/P3 high.

Except it didn't work. Not through the M5Unified API, not through raw I²C writes with the correct register map (which, by the way, is documented incorrectly in most online sources — the I/O Configuration register is at 0x07, not 0x03). A brute-force sweep of every pin on both expanders, HIGH and LOW, with a multimeter on J10 — nothing. Zero voltage change.

The software path to enabling VBUS was a dead end. Four sketch iterations, full register dumps, raw I²C — the MT9700 load switch wouldn't budge. Whether this is a silicon revision issue, an undocumented board-level quirk, or just a broken gate on this particular unit, we never determined.

The fix was a soldering iron. A short wire from a 5V pad on the Tab5 mainboard directly to the VCC pin on the USB-A connector, bypassing the MT9700 entirely. Warranty voided, problem solved, testing simplified enormously. Sometimes the fastest path through a hardware gate is through the hardware.

For the IDF project, we still include the I²C expander writes as a best-effort VBUS enable — they might work on other Tab5 units or future board revisions. But the soldered bypass is what actually powers the dongles in our test setup.

---

## Problem 2: Classification — What's Behind the Default Config?

Both dongles — the ASIX AX88179B and the RTL8152 — boot into vendor-specific USB configurations by default. If you just plug them in and read the active configuration descriptor, all you see is `bInterfaceClass=0xFF` (vendor-specific) with opaque endpoints. The class-compliant interfaces are hiding in alternate configurations that the host has to explicitly request.

ESP-IDF's `usb_host_get_active_config_descriptor()` only returns the cached active config. To see what else a device offers, you need raw `GET_DESCRIPTOR(Configuration, index)` control transfers. This required implementing a manual USB control transfer pipeline:

```c
usb_transfer_t *xfer;
usb_host_transfer_alloc(256, 0, &xfer);
// Fill setup packet for GET_DESCRIPTOR
xfer->device_handle = dev_hdl;
xfer->callback = ctrl_xfer_cb;
usb_host_transfer_submit_control(client_hdl, xfer);

// CRITICAL: pump events in a polling loop
while (!ctrl_xfer_complete) {
    usb_host_lib_handle_events(0, &event_flags);
    usb_host_client_handle_events(client_hdl, 0);
    vTaskDelay(1);
}
```

That polling loop is essential. The USB host stack is single-threaded in the event-processing sense. If you block waiting on a semaphore inside the same task that needs to process USB events, you deadlock — the transfer callback never fires because events never get processed. This was the cause of an early `hcd_urb_dequeue` Guru Meditation crash that took some debugging to trace.

With the multi-config descriptor reader working, both dongles revealed their full capabilities:

**ASIX AX88179B** (VID 0x0B95, PID 0x1790) — 3 configurations:

| Config | Class | Protocol |
|--------|-------|----------|
| 1 (default) | 0xFF Vendor-specific | Proprietary |
| 2 | 0x02/0x0D | **CDC-NCM** (Network Control Model) |
| 3 | 0x02/0x06 | **CDC-ECM** (Ethernet Control Model) |

**Realtek RTL8152** (VID 0x0BDA, PID 0x8152) — 2 configurations:

| Config | Class | Protocol |
|--------|-------|----------|
| 1 (default) | 0xFF Vendor-specific | Proprietary |
| 2 | 0x02/0x06 | **CDC-ECM** |

Both dongles are class-compliant — they just hide it behind `SET_CONFIGURATION`. The ASIX even offers both NCM and ECM. The "bad" RTL8152 dongle turned out to be useful after all.

---

## Problem 3: The Arduino Dead End

The natural next step was to switch the dongle to its class-compliant configuration and claim the ECM interfaces. This hit a wall in Arduino.

ESP-IDF has a feature called `enum_filter_cb` — a callback that fires during USB enumeration and lets you choose which configuration the host stack caches. You set it via `usb_host_config_t.enum_filter_cb`. The code compiled fine in Arduino. The callback was never called.

The reason: `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` is disabled in the M5Stack Arduino BSP. The field exists in the header (so code compiles), but IDF silently skips calling the callback at runtime. Without this, the host stack caches config 1 (vendor-specific), and any attempt to claim the ECM interfaces fails with `ESP_ERR_NOT_FOUND`.

Then came the second blocker. Even if the filter worked, USB 2.0 High-Speed bulk endpoints are fixed at 512 bytes Maximum Packet Size. The Arduino BSP's HCD is compiled with a limit of 256 bytes:

```
E HCD DWC: EP MPS (512) exceeds supported limit (256)
E USBH: EP Alloc error: ESP_ERR_NOT_SUPPORTED
```

Both options are baked into the precompiled BSP at build time. You cannot change them from an Arduino sketch. The Arduino path was a dead end for USB Ethernet.

---

## Problem 4: The IDF Pivot

The solution was to build a native ESP-IDF project. This gave us full control over `sdkconfig` and access to Espressif's managed components:

- **`espressif/iot_usbh_ecm` v0.2.1** — CDC-ECM host driver
- **`espressif/iot_usbh_cdc` v3.0.0** — CDC base driver (required dependency)
- **`espressif/iot_eth` v1.0.0** — `esp_netif` glue layer

One important discovery: **no CDC-NCM driver exists** in the Espressif component registry. NCM constants are defined in headers but there's no implementation. The workaround: the ASIX AX88179B has CDC-ECM in config 3 (documented during Phase 1 classification), so both dongles could be handled by the same ECM component.

The core architecture is a known-dongle table paired with the enum filter callback:

```c
static const known_dongle_t KNOWN_DONGLES[] = {
    { 0x0BDA, 0x8152, 2, "Realtek RTL8152 (ECM in cfg 2)" },
    { 0x0B95, 0x1790, 3, "ASIX AX88179B (ECM in cfg 3)" },
};

static bool eth1_enum_filter_cb(
    const usb_device_desc_t *dev_desc,
    uint8_t *bConfigurationValue)
{
    for (size_t i = 0; i < NUM_KNOWN_DONGLES; i++) {
        if (dev_desc->idVendor == KNOWN_DONGLES[i].vid &&
            dev_desc->idProduct == KNOWN_DONGLES[i].pid) {
            *bConfigurationValue =
                KNOWN_DONGLES[i].ecm_bConfigurationValue;
            return true;
        }
    }
    *bConfigurationValue = 1; // unknown: keep default
    return true;
}
```

The filter fires at enumeration time, tells IDF to cache the ECM configuration instead of the vendor one, and the `iot_usbh_ecm` component takes it from there — claiming interfaces, selecting alt settings, and wiring frames through to `esp_netif`. The first successful run was almost anticlimactic:

```
Filter: ASIX AX88179B (ECM in cfg 3) -> use config 3
ECM device found
ECM link UP: 1000 Mbps full-duplex
GOT IP via USB Ethernet:
    IP:      192.168.0.154
    Mask:    255.255.255.0
    Gateway: 192.168.0.1
```

DHCP lease in about one second. Full 1 Gbps link negotiation. IPv6 link-local address auto-assigned. The entire networking stack — ARP, IP, TCP, UDP, DHCP, DNS — all inherited from `esp_netif` and lwIP, zero code from us.

One gotcha along the way: `iot_usbh_cdc` (the base CDC driver) must be installed *before* `iot_eth_new_usb_ecm()`, with `skip_init_usb_host_driver = true` since we already installed the USB host stack ourselves (to get our enum filter in place). Without this, you get a cryptic `ESP_ERR_INVALID_STATE` crash at boot.

Another: ESP-IDF 5.5.x dropped support for ESP32-P4 eco2 silicon (chip revision v1.x). The Tab5 has revision v1.3. Flashing an IDF 5.5 binary produces `chip revision v3.1 required, chip is v1.3`. The fix is ESP-IDF v5.4.3, which supports v0.0 through v1.99.

---

## Problem 5: Throughput — From 0.18 to 13 Mbps

With packets flowing, the natural question was: how fast? The first measurement was sobering.

**Baseline: 0.18 Mbps.** One megabyte from a speed test server took 47 seconds. On a link negotiated at 1 Gbps.

The bottleneck was lwIP's default TCP configuration. Out of the box, ESP-IDF configures TCP windows at approximately 5.7 KB with no window scaling. Over any path with non-trivial round-trip time, this caps throughput at a few hundred kilobits per second regardless of link speed — the receiver can't advertise enough buffer space to keep the pipe full.

### Round 1: lwIP Tuning

```
CONFIG_LWIP_TCP_WND_DEFAULT=65535
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535
CONFIG_LWIP_TCP_WND_SCALE=y
CONFIG_LWIP_TCP_RCV_SCALE=3
CONFIG_LWIP_TCP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCP_MSS=1440
CONFIG_LWIP_TCP_MSL=5000
CONFIG_LWIP_IRAM_OPTIMIZATION=y
```

64 KB windows with scale factor 3 (effective ~512 KB), larger mailboxes, and lwIP hot paths moved to IRAM. Result: **6.85 Mbps** — a 38× improvement from one sdkconfig change.

### Round 2: The Aggressive Failure

Encouraged, we pushed harder: pin the TCP/IP task to CPU1, enable `TCP_OVERSIZE_MSS`, increase mailbox sizes further. The dongle never enumerated. The USB host task, running on CPU0, was starved of cycles at boot. Power cycle required to recover. All changes reverted.

### Round 3: The Selective Failure

More cautiously, we tried individual settings from Round 2 without the CPU affinity change: `TCP_OVERSIZE_MSS`, `TCP_SACK_OUT`, `TCP_QUEUE_OOSEQ`, bigger PBUF pool. Single-stream throughput *dropped* from 7.29 to 4.79 Mbps. More memory allocation overhead without enough benefit. Reverted.

### The Parallel Breakthrough

The real insight came from testing parallel connections. With pure Round 1 settings and 4 concurrent raw TCP downloads:

| Connections | Speed | Notes |
|-------------|-------|-------|
| 1× (esp_http_client) | 6.78 Mbps | Real-world single-stream baseline |
| **4× (raw TCP)** | **13.00 Mbps** | Sweet spot — 2× single stream |
| 8× (raw TCP) | 3.62 Mbps | Overwhelms USB pipe, collapses |

Four parallel connections doubled throughput. Eight connections *destroyed* it — the workers fight over the single CDC-ECM USB pipe, causing packet loss, retransmissions, and massive stalls.

The 4× sweet spot confirmed that the bottleneck was TCP congestion control, not the USB link. Each individual TCP stream is limited by slow-start and congestion windows; running four streams in parallel fills the pipe without overwhelming it. Going beyond four exceeds the USB CDC-ECM protocol's ability to multiplex — each Ethernet frame (1514 bytes) requires 3+ USB bulk transactions at 512 bytes MPS, and there's only so much scheduling headroom.

One other optimization mattered: **task priorities**. The USB host library task was raised to priority 7 (above default), while the speed test tasks ran at priority 3-4. This ensures the USB event processing loop is never starved by network activity — the exact problem that killed Round 2.

### The Dongle Gap

Running the same test suite on both dongles revealed a significant difference:

| Test | ASIX AX88179B | RTL8152 |
|------|--------------|---------|
| 1× single stream | 6.78 Mbps | 6.29 Mbps |
| 4× parallel | **13.00 Mbps** | 2.99 Mbps |

The RTL8152 performs comparably in single-stream, but collapses under parallel load. Its CDC-ECM implementation is likely minimal — checkbox compliance for Nintendo Switch compatibility, with the vendor-specific protocol (driven by Linux's `r8152` driver) being the real performance path. The ASIX's ECM implementation is more robust, scaling linearly to 4 connections.

---

## Where We Landed

Starting from zero — no USB Ethernet support on the ESP32-P4 Arduino platform, a dongle bought on bad advice, and a power gate nobody had documented — we ended up with:

- **Both dongles working** over CDC-ECM through a single codebase
- **DHCP in ~1 second**, full 1 Gbps link negotiation
- **13 Mbps peak throughput** on the ASIX (72× over the untuned baseline)
- **6.78 Mbps single-stream** (real-world, what any application sees)
- A known-dongle table and enum filter pattern that's extensible to any future CDC-ECM device

The theoretical ceiling for CDC-ECM over USB 2.0 HS is roughly 20-25 Mbps (one 1514-byte frame per bulk transfer, protocol overhead per transaction). Achieving 13 Mbps — about 60% of theoretical — is a solid result for a class-compliant driver with no vendor shortcuts.

CDC-NCM would do better (it batches multiple Ethernet frames per USB transfer), but there's no Espressif driver for it yet. When one appears, the architecture is ready — it's the same `iot_eth` glue layer, same `esp_netif` binding, just a different frame-level transport.

---

## Lessons for the Next Person

**Target class-compliant protocols.** When building a USB device driver library, default to USB-IF standard class protocols over vendor-specific ones unless there's an explicit external requirement otherwise. One driver, many vendors, no legal risk, no vendor lock-in. This is true independent of any policy environment.

**Read the descriptors, don't guess.** Both dongles in this project had class-compliant interfaces hiding behind vendor-specific defaults. You can't know from a part number alone — you have to read all configuration descriptors via raw `GET_DESCRIPTOR` control transfers.

**Arduino BSP limits are real.** The M5Stack Arduino BSP has `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` disabled and an HCD MPS limit of 256 bytes. Both are baked in at compile time. If you need full USB 2.0 HS bulk support or the enum filter callback, you need native ESP-IDF.

**Don't block in the USB event loop.** The USB host stack's single-threaded event processing means any blocking wait (semaphore, long delay) inside the task that pumps `usb_host_lib_handle_events()` will deadlock transfers. Use polling loops instead.

**lwIP defaults are terrible for throughput.** ESP-IDF ships with ~5.7 KB TCP windows. Bump to 64 KB with window scaling and you get a 38× improvement for free. But be surgical with further tuning — aggressive settings can starve the USB host task and prevent enumeration entirely.

**Parallel connections reveal the real ceiling.** A single TCP stream is congestion-window limited. Four parallel connections doubled throughput on the ASIX dongle. Eight connections overwhelmed the USB pipe. The sweet spot depends on the protocol (ECM vs NCM) and the dongle's firmware quality.

**"Nintendo Switch compatible" is a reliable purchasing heuristic.** The Switch has no user-installable driver path. If a dongle works on it, it must be class-compliant. This is a stronger signal than any chipset claim on a product listing.

---

## The Library

All of this is now wrapped in a reusable `USBEth` component with an Arduino-friendly API. Three lines to get online:

```cpp
#include <USBEth.h>

void setup() {
    USBEth.begin();
    while (!USBEth.hasIP()) delay(500);
    // You have internet. Do whatever you want.
}
```

It builds as an ESP-IDF project with `arduino-esp32` as a managed component — full `sdkconfig` control for the USB host stack and lwIP tuning, Arduino APIs for everything else. The component handles VBUS power, USB enumeration with config selection, CDC-ECM bringup, and `esp_netif` integration. The dongle table is extensible. The event system is optional.

Getting this to compile required its own set of discoveries: managed components register as `espressif__arduino-esp32` (not `arduino`) in CMake, arduino-esp32's `NetworkClientSecure` has a broken build guard around mbedTLS PSK (sidestepped by using IDF-native `esp_http_client`), and the default 1 MB partition is too small for arduino-esp32's binary footprint (custom partition table with 2 MB app partitions).

The $7 RTL8152 dongle that started this whole project? It works fine at 6 Mbps over CDC-ECM. Not the fastest. Not the recommended choice. But it works — which is more than anyone expected when the plan was to port a 4,000-line GPL vendor driver.

Sometimes the right answer really is a $15 ASIX dongle and a public specification.

---

*Built on M5Stack Tab5 (ESP32-P4 eco2, chip rev v1.3), ESP-IDF v5.4.3, with `espressif/iot_usbh_ecm` v0.2.1. Source code: https://github.com/magicsmokepress/usb-eth-4-esp32p4. April 2026.*
