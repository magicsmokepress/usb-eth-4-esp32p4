# Technical Diary: USB Ethernet on M5Stack Tab5 (ESP32-P4)

Raw notes on every dead end, error message, and config change. If you're trying to do this yourself, this is the document that saves you the hours we already spent.

---

## Day 1 — Phase 0/1: Arduino enumeration sketches

### Starting point

M5Stack Tab5, ESP32-P4 eco2 silicon (chip revision v1.3). Board package M5Stack 3.2.6 in Arduino IDE. Two USB Ethernet dongles:

- "Fake RTL8152" — actually a real Realtek RTL8152 (VID 0x0BDA, PID 0x8152)
- "Nintendo Switch compatible" ASIX AX88179B (VID 0x0B95, PID 0x1790)

Goal: enumerate both, read their USB configuration descriptors, classify whether they have class-compliant CDC-ECM or CDC-NCM interfaces.

### VBUS disaster

Tab5 USB-A (J10) VBUS is gated by MT9700 load switch. Enable pin is `USB5V_EN` on IO expander U7 (PI4IOE5V6408 @ I2C 0x44), pin P3. Second relevant pin: `EXT5V_EN` on U6 (@ 0x43), pin P2.

**PI4IOE5V6408 register map (corrected — online sources are WRONG):**

| Register | Address | Function |
|----------|---------|----------|
| Input Port | 0x01 | Read pin states (RO) |
| Output Port | 0x03 | Set output levels |
| Polarity Inversion | 0x05 | Invert input readback |
| I/O Configuration | **0x07** | 0=output, 1=input (NOT 0x03 as commonly listed) |

Four iterations of VBUS sketches:
1. M5Unified API: `M5.getIOExpander(1).setDirection(3, false); M5.getIOExpander(1).digitalWrite(3, true);` — writes succeed, readback confirms, J10 VCC stays at 0V.
2. Raw Wire I2C with correct register map (0x07 for config, 0x03 for output) — same result.
3. Brute-force: sweep ALL pins on BOTH expanders, HIGH and LOW, 1-second dwell each, multimeter on J10 — zero voltage change on any combination.
4. `M5.In_I2C` API — returns all zeros. Useless for expander reads. Use `Wire` on GPIO 31 (SDA) / 32 (SCL) instead.

**Important:** `Wire.begin(31, 32, 400000)` must be called explicitly. `M5.begin()` does NOT initialize Arduino Wire.

**Resolution: soldered a wire from a 5V pad on the Tab5 mainboard directly to J10 VCC.** Bypasses MT9700 entirely. Warranty voided. Problem eliminated permanently.

### The `usb_host_get_config_descriptor` doesn't exist

First attempt to read non-active configuration descriptors:

```
error: 'usb_host_get_config_descriptor' was not declared in this scope
```

ESP-IDF only has `usb_host_get_active_config_descriptor()`. That returns whichever config was cached at enumeration (always config 1, vendor-specific). To read config 2 or 3, you need raw `GET_DESCRIPTOR(Configuration, index)` control transfers via `usb_host_transfer_submit_control()`.

### The semaphore deadlock (Guru Meditation)

First implementation of the control transfer used a semaphore pattern:

```c
// WRONG — deadlocks
xSemaphoreTake(xfer_done_sem, portMAX_DELAY);
```

Crash: `Guru Meditation Error: Core 0 panic (LoadProhibited). Exception in hcd_urb_dequeue`

Root cause: the USB host stack processes transfer callbacks in `usb_host_lib_handle_events()` and `usb_host_client_handle_events()`. If you block on a semaphore in the same task, those functions never run, the callback never fires, and you deadlock. The crash happens when the HCD times out and tries to dequeue an URB that's in an inconsistent state.

**Fix: event-pumping polling loop:**

```c
volatile bool ctrl_xfer_complete = false;

// In callback:
ctrl_xfer_complete = true;

// In calling code:
while (!ctrl_xfer_complete) {
    usb_host_lib_handle_events(0, &event_flags);
    usb_host_client_handle_events(client_hdl, 0);
    vTaskDelay(1);
}
```

This is the single most important pattern for raw USB control transfers on ESP-IDF when you're running a single-task USB host. Non-blocking event processing interleaved with your wait loop.

### The `xfer_done_sem` stale reference

After refactoring to the polling loop, a stale `xfer_done_sem` reference remained in `print_descriptors()`:

```
error: 'xfer_done_sem' was not declared in this scope
```

Leftover code from the semaphore approach. Removed the block.

### Classification results

With multi-config descriptor reading working:

**ASIX AX88179B** — 3 USB configurations:

| Config | bConfigValue | Class | SubClass | Protocol |
|--------|-------------|-------|----------|----------|
| 0 (default) | 1 | 0xFF | 0xFF | Vendor-specific |
| 1 | 2 | 0x02 | 0x0D | **CDC-NCM** |
| 2 | 3 | 0x02 | 0x06 | **CDC-ECM** |

CDC-specific descriptors found: CDC Header, CDC Union (ctrl=0, data=1), ECM Functional (iMACAddress=5, MSS=1514), NCM Functional (ncmVersion=1.00).

**RTL8152** — 2 USB configurations:

| Config | bConfigValue | Class | SubClass | Protocol |
|--------|-------------|-------|----------|----------|
| 0 (default) | 1 | 0xFF | 0xFF | Vendor-specific |
| 1 | 2 | 0x02 | 0x06 | **CDC-ECM** |

Both class-compliant. Both hiding behind vendor-specific config 1.

---

## Day 1 continued — Phase 2.1: SET_CONFIGURATION

Sketch: `phase2/Tab5_USB_SetConfig/Tab5_USB_SetConfig.ino`

Auto-classification logic: prefer NCM over ECM (NCM supports packet aggregation). ASIX gets `SET_CONFIGURATION(2)` for NCM, RTL8152 gets `SET_CONFIGURATION(2)` for ECM.

Raw control transfer to issue SET_CONFIGURATION, then GET_CONFIGURATION to verify:

```c
// SET_CONFIGURATION
USB_SETUP_PACKET_INIT_SET_CONFIG(xfer->data_buffer, config_value);
xfer->num_bytes = 0;  // no data stage
usb_host_transfer_submit_control(client_hdl, xfer);
// ... pump events ...

// GET_CONFIGURATION (verify)
setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD;
setup->bRequest = USB_B_REQUEST_GET_CONFIGURATION;
setup->wValue = 0;
setup->wIndex = 0;
setup->wLength = 1;
```

Both dongles switch. MAC addresses read from string descriptors. Green LED blinked briefly on first boot (Guru Meditation on that boot — race condition in early code, fixed by second boot).

### The `eth_proto_t` forward declaration problem

```
error: 'eth_proto_t' does not name a type
```

Arduino IDE auto-generates function prototypes by scanning the .ino file top-to-bottom. It inserts prototypes ABOVE typedefs. If a function parameter uses a typedef that's defined later in the file, the auto-prototype references a type that doesn't exist yet.

**Fix:** move all typedefs and struct definitions to the very top of the .ino file, before any function that uses them.

---

## Day 2 — Phase 2.2a: Arduino BSP hits the wall

Sketch: `phase2/Tab5_USB_LinkUp/Tab5_USB_LinkUp.ino`

After SET_CONFIGURATION, need to claim the ECM interfaces and start data transfer. Two hard blockers:

### Blocker 1: enum_filter_cb silently ignored

```c
usb_host_config_t host_cfg = {
    .enum_filter_cb = my_filter_cb,
};
usb_host_install(&host_cfg);
```

Code compiles. Callback never fires. No error, no warning. The `enum_filter_cb` field exists in the struct (defined in the header), but `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` is disabled in the M5Stack BSP's prebuilt libs. IDF's internal code has `#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` around the callback invocation. When the config is disabled, the code path that calls the callback simply doesn't exist in the compiled binary.

Consequence: IDF caches config 1 (vendor-specific) at enumeration time. Only interface 0 is present. Calling `usb_host_interface_claim()` for interface 1 returns `ESP_ERR_NOT_FOUND`.

### Blocker 2: HCD bulk MPS limit

```
E HCD DWC: EP MPS (512) exceeds supported limit (256)
E USBH: EP Alloc error: ESP_ERR_NOT_SUPPORTED
```

USB 2.0 High-Speed bulk endpoints have a fixed Maximum Packet Size of 512 bytes. The Arduino BSP's HCD is compiled with a maximum of 256. Both dongles' ECM bulk endpoints are 512 bytes.

Both limits are baked into the prebuilt BSP at compile time. Cannot be changed from an Arduino sketch. No workaround. Dead end.

---

## Day 2 continued — Phase 2.2: Pivot to native ESP-IDF

Decision: build a native ESP-IDF project with full sdkconfig control. Use Espressif's managed components instead of writing USB protocol code by hand.

### Component research

Searched the Espressif Component Registry:

| Component | Version | What it does |
|-----------|---------|-------------|
| `espressif/iot_usbh_ecm` | 0.2.1 | CDC-ECM host driver |
| `espressif/iot_usbh_cdc` | 3.0.0 | CDC base driver (required dep of ECM) |
| `espressif/iot_eth` | 1.0.0 | esp_netif glue layer |
| `espressif/iot_usbh_rndis` | exists | RNDIS host (not needed for this project) |
| CDC-NCM host driver | **DOES NOT EXIST** | NCM constants in headers but no implementation |

**Critical finding:** no CDC-NCM driver. Our ASIX dongle has NCM in config 2 and ECM in config 3. We point the ASIX at config 3 (ECM) instead of config 2 (NCM). Both dongles handled by the same `iot_usbh_ecm` component.

### Wrong type: `iot_usbh_ecm_match_id_t`

First attempt at the match list used a made-up type name:

```c
iot_usbh_ecm_match_id_t match_ids[] = { ... };  // WRONG
```

The real type is `usb_device_match_id_t` from `usbh_helper.h`:

```c
typedef struct {
    uint16_t match_flags;    // USB_DEVICE_ID_MATCH_VID_PID etc
    uint16_t idVendor;
    uint16_t idProduct;
    // ... more fields
} usb_device_match_id_t;
```

The list must be terminated by a sentinel entry with `match_flags = 0`. Found this by reading the actual managed component headers in the build directory.

### Missing include: `iot_eth_netif_glue.h`

```
warning: implicit declaration of function 'iot_eth_new_netif_glue'
```

The netif glue function lives in a separate header that's not included by `iot_eth.h`:

```c
#include "iot_eth_netif_glue.h"  // must be explicitly included
```

### ESP-IDF 5.5 won't flash: chip revision mismatch

First attempt used ESP-IDF 5.5.4:

```
ESP-ROM:esp32p4-20240606
Image requires chip rev >= v3.1, chip is v1.3
```

ESP-IDF 5.5 dropped support for ESP32-P4 eco2 silicon (revision v1.x). The `Kconfig` for ESP32-P4 in 5.5 only offers `CONFIG_ESP32P4_REV_MIN_3` and `CONFIG_ESP32P4_REV_MIN_31`. No option for v0 or v1. The minimum revision gets baked into the binary header and the ROM bootloader rejects it.

**Fix: install ESP-IDF v5.4.3.** This version supports `CONFIG_ESP32P4_REV_MIN_0=y` (covers v0.0 through v1.99).

### Accidentally installed 5.5 twice

First install attempt: ran the Espressif installer, selected "5.4" — but the installer defaulted to `esp-idf-v5.5.4-2`. Same chip revision error. Had to explicitly select the 5.4.3 branch.

After installing 5.4.3, the old `sdkconfig` (generated by 5.5) still existed. IDF uses the existing `sdkconfig` instead of `sdkconfig.defaults`.

**Fix: always delete sdkconfig before switching IDF versions:**

```powershell
Remove-Item sdkconfig -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force build
idf.py build
```

### PSRAM speed: 200 MHz → 120 MHz

Initial sdkconfig had `CONFIG_SPIRAM_SPEED_200M=y`. This is only available on ESP32-P4 eco3+ silicon. Tab5 with eco2 maxes out at 120 MHz:

```
CONFIG_SPIRAM_SPEED_120M=y
```

200 MHz compiled fine but would fail at runtime on the v1.3 chip.

### OneDrive locking build files

```
PermissionError: [WinError 32] The process cannot access the file
because it is being used by another process
```

`idf.py fullclean` failed because OneDrive was syncing files in the `build/` directory. Windows file locks.

**Fix:** pause OneDrive sync, then:

```powershell
Remove-Item -Recurse -Force build
```

### CDC base driver must be installed first

First successful flash with IDF 5.4.3, but crash at startup:

```
E usbh_cdc: usbh cdc not installed
ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
```

`iot_usbh_ecm` depends on `iot_usbh_cdc` (the CDC base driver). You must call `usbh_cdc_driver_install()` before `iot_eth_new_usb_ecm()`. And since we already installed the USB host stack ourselves (to get our enum filter in), the CDC driver must skip its own USB host install:

```c
usbh_cdc_driver_config_t cdc_cfg = {
    .task_stack_size = 4096,
    .task_priority   = 5,
    .task_coreid     = 0,
    .skip_init_usb_host_driver = true,  // WE already did this
};
ESP_ERROR_CHECK(usbh_cdc_driver_install(&cdc_cfg));
```

The ordering is: `usb_host_install()` → `usbh_cdc_driver_install()` → `iot_eth_new_usb_ecm()`.

### IT WORKS

After all that:

```
I (xxxx) eth1: Filter: ASIX AX88179B (ECM in cfg 3) -> use config 3
I (xxxx) iot_usbh_ecm: ECM device found
I (xxxx) iot_usbh_ecm: ECM link UP: 1000 Mbps full-duplex
I (xxxx) eth1: GOT IP: 192.168.0.154 / 255.255.255.0 / gw 192.168.0.1
```

DHCP lease in ~1 second. IPv6 link-local auto-assigned. Full 1 Gbps link negotiation. The entire TCP/IP stack inherited from esp_netif/lwIP with zero additional code.

---

## Day 3 — Phase 2.3: Throughput tuning

### Baseline measurement

Added `esp_http_client` speed test: download from `http://speed.cloudflare.com/__down?bytes=1048576` (1 MB).

```
Bytes:   1048576
Time:    47.23 s
Speed:   0.18 Mbps  (0.02 MB/s)
```

0.18 Mbps on a 1 Gbps link. Painful.

### Root cause: lwIP default TCP windows

Stock ESP-IDF lwIP config:

```
TCP_WND_DEFAULT = ~5744 bytes (~5.7 KB)
TCP_SND_BUF = ~5744 bytes
Window scaling: disabled
```

With a ~20ms RTT to Cloudflare's CDN edge, the bandwidth-delay product is `5744 * (1/0.020)` = 287 KB/s = 2.3 Mbps theoretical max. In practice even worse due to slow-start and congestion avoidance.

### Round 1 tuning — the 38× jump

```
CONFIG_LWIP_TCP_WND_DEFAULT=65535
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535
CONFIG_LWIP_TCP_WND_SCALE=y
CONFIG_LWIP_TCP_RCV_SCALE=3          # effective window: 64K × 8 = 512K
CONFIG_LWIP_TCP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCP_MSL=5000
CONFIG_LWIP_TCP_MSS=1440
CONFIG_LWIP_IRAM_OPTIMIZATION=y
```

Also switched to 10 MB download from Cloudflare (`bytes=10485760`) with 16 KB buffer to amortize TCP handshake and slow-start.

```
Bytes:   10485760
Time:    12.25 s
Speed:   6.85 Mbps  (0.86 MB/s)
```

**38× improvement from sdkconfig changes alone.**

### Round 2 — broke everything

Aggressive additions:

```
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y    # pin TCP/IP to core 1
CONFIG_LWIP_TCP_OVERSIZE_MSS=y
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64        # bigger mailboxes
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
```

Result: dongle never enumerated. The USB host library task runs on core 0 at priority 5. Pinning the TCP/IP task to core 1 at high priority (it's also at 5 or higher in some configs) starved the USB host task during the initial enumeration handshake. The USB host needs frequent event processing during enumeration — device reset, address assignment, descriptor read, config selection all happen in rapid sequence with tight timing.

Had to power cycle the Tab5 to recover. Reverted all changes.

**Lesson:** never pin network tasks to a specific core when USB host is running. The USB host event processing is latency-sensitive during enumeration.

### Round 3 — selective settings, still regressed

Tried individual round-2 settings without the CPU affinity:

```
CONFIG_LWIP_TCP_OVERSIZE_MSS=y
CONFIG_LWIP_TCP_QUEUE_OOSEQ=y
CONFIG_LWIP_TCP_SACK_OUT=y
CONFIG_LWIP_PBUF_POOL_SIZE=48
```

Single-stream dropped from 7.29 to 4.79 Mbps. More memory allocation pressure from the extra features without matching benefit for our workload. Reverted.

**Lesson:** more lwIP features ≠ more throughput. The extra code paths and memory allocation in SACK, OOSeq, and oversized segment allocation add overhead that exceeds their benefit on a USB CDC-ECM link with moderate RTT.

### The parallel connection discovery

Built a multi-connection test: N parallel raw TCP sockets each downloading `SPEED_BYTES / N` bytes from Cloudflare.

| Connections | Speed | Notes |
|-------------|-------|-------|
| 1× (esp_http_client) | 6.78 Mbps | Real-world single-stream |
| 4× (raw TCP) | **13.00 Mbps** | 2× single-stream |
| 8× (raw TCP) | 3.62 Mbps | Collapsed — USB pipe overwhelmed |

The 4× result confirmed the bottleneck is TCP congestion control, not the USB link. Each TCP stream is independently limited by slow-start and congestion windows. Four streams fill the pipe; eight saturate the USB CDC-ECM scheduler and cause packet loss.

**Important detail on raw TCP vs esp_http_client:** our hand-rolled raw TCP socket implementation was actually *slower* than `esp_http_client` for single connections (5.96 vs 7.29 Mbps). IDF's HTTP client is well-optimized. But for parallel, raw TCP was much better — `esp_http_client` has per-instance memory overhead that compounds badly at 8× (4.56 Mbps with 8× esp_http_client vs 13.79 Mbps with 8× raw TCP).

### Task priority matters

Original config: USB host library task at priority 5, speed test at priority 5. Both competing equally.

Fixed config:
- USB host library: **priority 7** (never starved)
- CDC driver: priority 5
- Speed test / download workers: **priority 3-4** (below USB)

This prevents the throughput test from starving USB event processing, which was a contributing factor in the round-2 failure.

### Dongle comparison

Same firmware, same network, same test suite:

| Test | ASIX AX88179B | RTL8152 |
|------|--------------|---------|
| 1× single | 6.78 Mbps | 6.29 Mbps |
| 4× parallel | **13.00 Mbps** | 2.99 Mbps |
| 8× parallel | 3.62 Mbps | 2.51 Mbps |

RTL8152 collapses under parallel load. Worker completion times were wildly uneven (Worker 1 done in 5s, Worker 2 took 28s). The RTL8152's CDC-ECM firmware can't handle concurrent bulk transfer scheduling. Its real performance path is through the vendor-specific protocol that Linux's `r8152` driver uses.

ASIX scales linearly from 1× to 4× (6.78 → 13.00). Its ECM implementation is production-quality.

---

## Final working sdkconfig.defaults

The config that survived all the tuning rounds:

```ini
CONFIG_IDF_TARGET="esp32p4"
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP32P4_REV_MIN_0=y

CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_120M=y
CONFIG_SPIRAM_USE_MALLOC=y

CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y
CONFIG_USB_HOST_HUBS_SUPPORTED=y
CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=512

CONFIG_LWIP_TCP_WND_DEFAULT=65535
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535
CONFIG_LWIP_TCP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCP_MSL=5000
CONFIG_LWIP_TCP_MSS=1440
CONFIG_LWIP_TCP_WND_SCALE=y
CONFIG_LWIP_TCP_RCV_SCALE=3
CONFIG_LWIP_IRAM_OPTIMIZATION=y

CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

Everything that was added beyond this either broke USB enumeration, regressed throughput, or both.

---

## Key patterns / rules extracted

1. **Always delete sdkconfig when changing IDF versions or sdkconfig.defaults.** IDF prefers existing sdkconfig over defaults.

2. **Never block in the USB event loop task.** Use polling loops with `usb_host_lib_handle_events(0, ...)` + `usb_host_client_handle_events(client, 0)` + `vTaskDelay(1)`.

3. **USB host task priority must be higher than network tasks.** Priority 7 for USB host, 3-4 for download workers. Otherwise enumeration fails under network load.

4. **Never pin lwIP TCPIP task to a specific core when USB host is active.** Starves USB event processing during enumeration.

5. **`CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y` is mandatory** for class-compliant dongles that default to vendor-specific config. Arduino BSP has this disabled. Must use IDF.

6. **`iot_usbh_cdc` must be installed before `iot_eth_new_usb_ecm()`** with `skip_init_usb_host_driver = true` if you installed the USB host stack yourself.

7. **Match list sentinel: `match_flags = 0`.** The `usb_device_match_id_t` array must end with a zero-initialized entry.

8. **Arduino IDE auto-generates function prototypes above typedefs.** Move typedefs to top of .ino file or use a .h file.

9. **PI4IOE5V6408 I/O Config register is 0x07, not 0x03.** Online sources are wrong. 0x03 is the Output Port register.

10. **ESP-IDF 5.5+ does not support ESP32-P4 eco2 (v1.x).** Use 5.4.3. The `CONFIG_ESP32P4_REV_MIN_0=y` option only exists in 5.4.x.

11. **PSRAM 200 MHz is eco3+ only.** Tab5 eco2 maxes at `CONFIG_SPIRAM_SPEED_120M=y`.

12. **Pause OneDrive before `idf.py fullclean` on Windows.** File locking causes PermissionError.

13. **4 parallel TCP connections is the CDC-ECM sweet spot.** Below that, TCP congestion control limits throughput. Above that, USB pipe contention causes packet loss.

14. **More lwIP features ≠ more throughput.** SACK, OOSeq, TCP_OVERSIZE_MSS all regressed performance on this platform. Stick with big windows + window scaling + IRAM optimization.

---

## Managed component versions (proven working)

```yaml
dependencies:
  idf: ">=5.4.0,<5.5.0"
  espressif/iot_usbh_ecm: "^0.2.1"
  espressif/iot_usbh_cdc: "^3.0.0"
  espressif/iot_eth: "*"
```

---

## Init sequence (correct ordering)

```
1. I2C bus init (GPIO 31 SDA, 32 SCL, 400 kHz)
2. Expander U7/P3 HIGH (USB5V_EN) — best effort
3. Expander U6/P2 HIGH (EXT5V_EN) — best effort
4. vTaskDelay(200ms)
5. esp_netif_init()
6. esp_event_loop_create_default()
7. esp_event_handler_register(IP_EVENT)
8. usb_host_install() with enum_filter_cb
9. xTaskCreate(usb_lib_task) at priority 7 on core 0
10. usbh_cdc_driver_install() with skip_init_usb_host_driver=true
11. iot_eth_new_usb_ecm() with match_id_list
12. iot_eth_install()
13. esp_netif_new() with ESP_NETIF_NETSTACK_DEFAULT_ETH
14. iot_eth_new_netif_glue()
15. esp_netif_attach()
16. iot_eth_start()
```

Swap any of steps 8-10 and you get either a silent failure or an `ESP_ERR_INVALID_STATE` crash.

---

---

## Day 4 — Phase 3: USBEth Arduino component

### Goal
Wrap the Phase 2 proven code into a reusable `USBEth` component with Arduino-like API, built as an IDF project with `espressif/arduino-esp32` as a managed component.

### Build error 1: `component 'arduino' could not be found`
The managed component `espressif/arduino-esp32` registers in CMake as `espressif__arduino-esp32` (namespace prefix), NOT `arduino`. Our `CMakeLists.txt` had `REQUIRES arduino`. Fix: removed `arduino` from REQUIRES entirely. The include paths resolve through the Component Manager via `idf_component.yml` dependencies.

Diagnostic that found it:
```
grep -rn "REQUIRES.*arduino" --include="CMakeLists.txt"
```
→ `main/CMakeLists.txt` was the culprit, not the component.

### Build error 2: `undefined reference to 'ssl_init(sslclient_context*)'`
`arduino-esp32` always compiles `NetworkClientSecure.cpp` which calls `ssl_init()`. But `ssl_client.cpp` wraps its entire body in `#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)`. Without PSK ciphersuites in mbedTLS config, the function compiles to nothing while the caller still references it.

Attempted fix: `CONFIG_MBEDTLS_KEY_EXCHANGE_PSK=y` — didn't work, the Kconfig option doesn't map to the right internal macro.

Working fix: replaced Arduino `HTTPClient` with IDF-native `esp_http_client` in the example. Nothing references `NetworkClientSecure` anymore, so the linker doesn't pull in the broken object file.

### Build error 3: `app partition is too small for binary`
`arduino-esp32` bloats the binary to ~1.1 MB. Default partition table has 1 MB app partition. Fix: custom `partitions.csv` with 2 MB app partitions + `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` (Tab5 has 16 MB flash).

### Runtime issue: `Serial.println()` output invisible
Arduino's `Serial` object on ESP32-P4 maps to UART0, but `idf.py monitor` connects via USB Serial JTAG. IDF's `ESP_LOGI` goes to USB JTAG (visible), Arduino's `Serial` goes to UART0 (invisible). Fix: replaced all `Serial.println()` with `ESP_LOGI()` and `printf()` in the example.

### First successful run
ASIX AX88179B, gigabit link, DHCP + HTTP test in 11.7 seconds total:
```
I (6721) USBEth: Filter: ASIX AX88179B (ECM in cfg 3) -> config 3
I (10041) iot_usbh_ecm: Notify - network connection changed: Connected
I (10073) iot_usbh_ecm: Notify - link speeds: 1000000 kbps ↑, 1000000 kbps ↓
I (11046) USBEth: Got IP: 192.168.0.154 / 255.255.255.0 gw 192.168.0.1
I (11762) main: HTTP 200: { "origin": "172.58.51.125" }
```

### Key pattern: arduino-esp32 as IDF component gotchas
1. Component name is `espressif__arduino-esp32`, not `arduino`
2. `NetworkClientSecure` has a broken mbedTLS dependency — avoid Arduino HTTP libs
3. Binary is >1 MB — need custom partition table
4. `Serial` ≠ USB JTAG on P4 — use `ESP_LOGI` or `printf` for monitor output
5. `CONFIG_AUTOSTART_ARDUINO=y` routes setup()/loop() through a FreeRTOS task, main_task exits immediately (this is normal)
6. `extern "C" {}` wrappers needed around C component headers in .cpp files

---

*April 2026. ESP-IDF 5.4.3. M5Stack Tab5 rev with ESP32-P4 eco2 v1.3.*
