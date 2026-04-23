/*
 * USBEth.cpp — USB Ethernet for ESP32-P4 via CDC-ECM
 *
 * Core logic extracted from phase2_idf/main/eth1_main.c (proven working).
 * Wrapped in Arduino-friendly USBEthClass.
 *
 * SPDX-License-Identifier: MIT
 */

#include "USBEth.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/i2c_master.h"

/* C headers from IDF + Espressif components — wrap for C++ linkage */
extern "C" {
#include "usb/usb_host.h"
#include "iot_usbh_ecm.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "usbh_helper.h"
}

/*
 * ABI SHIM: Our libusb.a was built from release/v5.4 HEAD which has a LARGER
 * usb_host_config_t than the M5Stack BSP's snapshot.  The BSP header gives a
 * 12-byte struct, but usb_host_install() reads 28 bytes.  We define the real
 * layout here and fill it manually in _install_usb_host().
 */
typedef struct {
    bool skip_phy_setup;
    bool root_port_unpowered;
    int  intr_flags;
    usb_host_enum_filter_cb_t enum_filter_cb;
    struct {
        uint32_t nptx_fifo_lines;
        uint32_t ptx_fifo_lines;
        uint32_t rx_fifo_lines;
    } fifo_settings_custom;
    unsigned peripheral_map;
} usb_host_config_real_t;

static const char *TAG = "USBEth";

/* ── Built-in dongle table (known class-compliant dongles) ─── */

static const usb_eth_dongle_t BUILTIN_DONGLES[] = {
    { 0x0BDA, 0x8152, 2, "Realtek RTL8152 (ECM in cfg 2)" },
    { 0x0B95, 0x1790, 3, "ASIX AX88179B (ECM in cfg 3)" },
    /* Add more known dongles here. Terminated by array size, not sentinel. */
};
#define NUM_BUILTIN_DONGLES (sizeof(BUILTIN_DONGLES) / sizeof(BUILTIN_DONGLES[0]))

/* ── Module state (single-instance) ──────────────────────────── */

static USBEthClass *s_instance = nullptr;
static iot_eth_handle_t s_eth_handle = nullptr;
static iot_eth_driver_t *s_ecm_driver = nullptr;

/* Active dongle table (points to built-in or user-provided) */
static const usb_eth_dongle_t *s_dongles = BUILTIN_DONGLES;
static size_t s_dongle_count = NUM_BUILTIN_DONGLES;

/* ── PI4IOE5V6408 I2C GPIO expander helpers ──────────────────── */

#define REG_OUT  0x03
#define REG_CFG  0x07

static esp_err_t expander_set_pin(i2c_master_dev_handle_t dev,
                                  uint8_t pin, bool level) {
    uint8_t bit = 1 << pin;
    uint8_t buf[2];
    uint8_t in;

    /* Configure as output */
    buf[0] = REG_CFG;
    if (i2c_master_transmit_receive(dev, buf, 1, &in, 1, 100) != ESP_OK)
        return ESP_FAIL;
    buf[0] = REG_CFG; buf[1] = in & ~bit;
    if (i2c_master_transmit(dev, buf, 2, 100) != ESP_OK)
        return ESP_FAIL;

    /* Set output level */
    buf[0] = REG_OUT;
    if (i2c_master_transmit_receive(dev, buf, 1, &in, 1, 100) != ESP_OK)
        return ESP_FAIL;
    buf[0] = REG_OUT; buf[1] = level ? (in | bit) : (in & ~bit);
    return i2c_master_transmit(dev, buf, 2, 100);
}

/* ── USB host lib event task ─────────────────────────────────── */

static void usb_lib_task(void *arg) {
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "No more USB clients");
        }
    }
}

/* ── Enumeration filter callback ─────────────────────────────── */

static bool enum_filter_cb(const usb_device_desc_t *dev_desc,
                           uint8_t *bConfigurationValue) {
    ESP_LOGI(TAG, "Enum filter: VID=0x%04X PID=0x%04X",
             dev_desc->idVendor, dev_desc->idProduct);
    for (size_t i = 0; i < s_dongle_count; i++) {
        if (dev_desc->idVendor == s_dongles[i].vid &&
            dev_desc->idProduct == s_dongles[i].pid) {
            *bConfigurationValue = s_dongles[i].ecm_config;
            ESP_LOGI(TAG, "Filter: %s -> config %d",
                     s_dongles[i].name, *bConfigurationValue);
            return true;
        }
    }
    /* Unknown: keep config 1, maybe it's already class-compliant */
    *bConfigurationValue = 1;
    ESP_LOGW(TAG, "Unknown dongle VID:0x%04X PID:0x%04X — using config 1",
             dev_desc->idVendor, dev_desc->idProduct);
    return true;
}

/* ═══════════════════════════════════════════════════════════════
 * USBEthClass implementation
 * ═══════════════════════════════════════════════════════════════ */

USBEthClass::USBEthClass()
    : _initialized(false)
    , _connected(false)
    , _has_ip(false)
    , _netif(nullptr)
    , _config{}
    , _event_cb(nullptr)
    , _event_cb_arg(nullptr)
    , _static_ip{}
    , _use_static_ip(false)
    , _hostname(nullptr)
{
    s_instance = this;
}

/* ── begin() with default Tab5 config ────────────────────────── */

bool USBEthClass::begin() {
    usb_eth_config_t cfg = USB_ETH_CONFIG_TAB5();
    return begin(cfg);
}

/* ── begin() with custom config ──────────────────────────────── */

bool USBEthClass::begin(const usb_eth_config_t &config) {
    if (_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    _config = config;

    /* Use custom dongle table if provided */
    if (_config.dongles && _config.dongle_count > 0) {
        s_dongles = _config.dongles;
        s_dongle_count = _config.dongle_count;
    } else {
        s_dongles = BUILTIN_DONGLES;
        s_dongle_count = NUM_BUILTIN_DONGLES;
    }

    ESP_LOGI(TAG, "Initializing USB Ethernet (%d known dongles)", (int)s_dongle_count);

    /* Step 1: VBUS enable (best-effort) */
    _enable_vbus();

    /* Step 2: networking core */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, ESP_EVENT_ANY_ID, _ip_event_handler, this));

    /* Step 3: USB host stack */
    if (!_install_usb_host()) return false;

    /* Step 4: ECM driver */
    if (!_start_ecm()) return false;

    _initialized = true;
    ESP_LOGI(TAG, "Ready. Plug in a USB Ethernet dongle.");
    return true;
}

/* ── end() ───────────────────────────────────────────────────── */

void USBEthClass::end() {
    if (!_initialized) return;

    if (s_eth_handle) {
        iot_eth_stop(s_eth_handle);
        /* Note: full teardown of USB host + ECM is complex;
         * for now just stop the eth driver. Full cleanup TBD. */
        s_eth_handle = nullptr;
    }

    _initialized = false;
    _connected = false;
    _has_ip = false;
    ESP_LOGI(TAG, "Stopped");
}

/* ── State queries ───────────────────────────────────────────── */

bool USBEthClass::connected() const { return _connected; }
bool USBEthClass::hasIP() const { return _has_ip; }

IPAddress USBEthClass::localIP() const {
    if (!_netif) return IPAddress();
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(_netif, &info);
    return IPAddress(info.ip.addr);
}

IPAddress USBEthClass::subnetMask() const {
    if (!_netif) return IPAddress();
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(_netif, &info);
    return IPAddress(info.netmask.addr);
}

IPAddress USBEthClass::gatewayIP() const {
    if (!_netif) return IPAddress();
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(_netif, &info);
    return IPAddress(info.gw.addr);
}

IPAddress USBEthClass::dnsIP(uint8_t idx) const {
    if (!_netif) return IPAddress();
    esp_netif_dns_info_t dns;
    esp_netif_dns_type_t type = (idx == 0) ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP;
    esp_netif_get_dns_info(_netif, type, &dns);
    return IPAddress(dns.ip.u_addr.ip4.addr);
}

String USBEthClass::macAddress() const {
    if (!_netif) return String("00:00:00:00:00:00");
    uint8_t mac[6];
    esp_netif_get_mac(_netif, mac);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

uint32_t USBEthClass::linkSpeed() const {
    /* TODO: extract from ECM link notification if exposed by iot_usbh_ecm */
    return _connected ? 1000 : 0;
}

esp_netif_t *USBEthClass::netif() const { return _netif; }

/* ── Event callback ──────────────────────────────────────────── */

void USBEthClass::onEvent(usb_eth_event_cb_t cb, void *arg) {
    _event_cb = cb;
    _event_cb_arg = arg;
}

void USBEthClass::_fire_event(usb_eth_event_t event) {
    if (_event_cb) _event_cb(event, _event_cb_arg);
}

/* ── Static IP config ────────────────────────────────────────── */

void USBEthClass::config(IPAddress ip, IPAddress gateway, IPAddress subnet,
                         IPAddress dns1, IPAddress dns2) {
    _use_static_ip = true;
    _static_ip.ip.addr = (uint32_t)ip;
    _static_ip.gw.addr = (uint32_t)gateway;
    _static_ip.netmask.addr = (uint32_t)subnet;
    /* DNS will be set after netif is created */
}

void USBEthClass::setHostname(const char *hostname) {
    _hostname = hostname;
}

/* ── Internal: VBUS enable ───────────────────────────────────── */

bool USBEthClass::_enable_vbus() {
    if (_config.vbus_sda == 0 && _config.vbus_scl == 0) {
        ESP_LOGI(TAG, "VBUS: skipping (no I2C config)");
        return true;
    }

    ESP_LOGI(TAG, "VBUS: enabling via I2C expander (best-effort)");

    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = (gpio_num_t)_config.vbus_sda;
    bus_cfg.scl_io_num = (gpio_num_t)_config.vbus_scl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "VBUS: I2C bus init failed (already in use?)");
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = 400000;

    /* Primary: USB5V_EN */
    if (_config.vbus_expander_addr) {
        i2c_master_dev_handle_t dev = nullptr;
        dev_cfg.device_address = _config.vbus_expander_addr;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            expander_set_pin(dev, _config.vbus_pin, true);
            ESP_LOGI(TAG, "VBUS: USB5V_EN set HIGH (0x%02X P%d)",
                     _config.vbus_expander_addr, _config.vbus_pin);
        }
    }

    /* Optional: EXT5V_EN */
    if (_config.ext5v_expander_addr) {
        i2c_master_dev_handle_t dev = nullptr;
        dev_cfg.device_address = _config.ext5v_expander_addr;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            expander_set_pin(dev, _config.ext5v_pin, true);
            ESP_LOGI(TAG, "VBUS: EXT5V_EN set HIGH (0x%02X P%d)",
                     _config.ext5v_expander_addr, _config.ext5v_pin);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(200)); /* settling time */
    return true;
}

/* ── Internal: USB host stack ────────────────────────────────── */

bool USBEthClass::_install_usb_host() {
    ESP_LOGI(TAG, "Installing USB host stack");

    /*
     * Use the REAL struct layout that matches our libusb.a (built from
     * IDF release/v5.4 HEAD).  The M5Stack BSP header defines a smaller
     * usb_host_config_t (12 bytes) but our usb_host_install() reads the
     * full 28-byte struct including peripheral_map and fifo_settings_custom.
     */
    usb_host_config_real_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.root_port_unpowered = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    host_cfg.enum_filter_cb = enum_filter_cb;
    host_cfg.peripheral_map = 0;  /* 0 = default to first USB peripheral */
    /* fifo_settings_custom stays zero = use defaults */

    esp_err_t err = usb_host_install((const usb_host_config_t *)&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return false;
    }

    /* USB lib task at high priority so it never gets starved */
    BaseType_t r = xTaskCreatePinnedToCore(
        usb_lib_task, "usb_lib", 4096, nullptr, 7, nullptr, 0);
    return (r == pdPASS);
}

/* ── Internal: ECM driver bringup ────────────────────────────── */

bool USBEthClass::_start_ecm() {
    /* Build match list from active dongle table */
    static usb_device_match_id_t match_ids[16] = {}; /* max 15 dongles + sentinel */
    size_t n = (s_dongle_count < 15) ? s_dongle_count : 15;
    memset(match_ids, 0, sizeof(match_ids));
    for (size_t i = 0; i < n; i++) {
        match_ids[i].match_flags = USB_DEVICE_ID_MATCH_VID_PID;
        match_ids[i].idVendor    = s_dongles[i].vid;
        match_ids[i].idProduct   = s_dongles[i].pid;
    }
    /* match_ids[n] stays zero = sentinel */

    /* CDC base driver */
    ESP_LOGI(TAG, "Installing CDC base driver");
    usbh_cdc_driver_config_t cdc_cfg = {};
    cdc_cfg.task_stack_size = 4096;
    cdc_cfg.task_priority   = 5;
    cdc_cfg.task_coreid     = 0;
    cdc_cfg.skip_init_usb_host_driver = true;

    esp_err_t err = usbh_cdc_driver_install(&cdc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CDC driver install failed: %s", esp_err_to_name(err));
        return false;
    }

    /* ECM driver */
    ESP_LOGI(TAG, "Creating ECM driver");
    iot_usbh_ecm_config_t ecm_cfg = {};
    ecm_cfg.match_id_list = match_ids;

    err = iot_eth_new_usb_ecm(&ecm_cfg, &s_ecm_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ECM driver create failed: %s", esp_err_to_name(err));
        return false;
    }

    /* iot_eth install */
    iot_eth_config_t eth_cfg = {};
    eth_cfg.driver = s_ecm_driver;
    eth_cfg.stack_input = nullptr;

    err = iot_eth_install(&eth_cfg, &s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_eth install failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Create esp_netif */
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t netif_cfg = {};
    netif_cfg.base = &base_cfg;
    netif_cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;

    _netif = esp_netif_new(&netif_cfg);
    if (!_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return false;
    }

    /* Hostname */
    if (_hostname) {
        esp_netif_set_hostname(_netif, _hostname);
    }

    /* Static IP */
    if (_use_static_ip) {
        esp_netif_dhcpc_stop(_netif);
        esp_netif_set_ip_info(_netif, &_static_ip);
    }

    /* Glue layer */
    iot_eth_netif_glue_handle_t glue = iot_eth_new_netif_glue(s_eth_handle);
    err = esp_netif_attach(_netif, glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Start */
    ESP_LOGI(TAG, "Starting ECM driver — waiting for dongle...");
    return (iot_eth_start(s_eth_handle) == ESP_OK);
}

/* ── IP event handler ────────────────────────────────────────── */

void USBEthClass::_ip_event_handler(void *arg, esp_event_base_t base,
                                    int32_t id, void *data) {
    USBEthClass *self = static_cast<USBEthClass *>(arg);
    if (!self) return;

    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(data);
        self->_connected = true;
        self->_has_ip = true;

        ESP_LOGI(TAG, "Got IP: " IPSTR " / " IPSTR " gw " IPSTR,
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));

        self->_fire_event(USB_ETH_EVENT_GOT_IP);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        self->_has_ip = false;
        ESP_LOGW(TAG, "Lost IP");
        self->_fire_event(USB_ETH_EVENT_LOST_IP);
    }
}

/* ── Global singleton ────────────────────────────────────────── */

USBEthClass USBEth;
