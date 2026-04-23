/*
 * USBEth — USB Ethernet for ESP32-P4 via CDC-ECM
 *
 * Wraps Espressif's iot_usbh_ecm + iot_eth into an Arduino-friendly API.
 * Requires ESP-IDF 5.4.x with arduino-esp32 as component.
 *
 * Usage:
 *   #include <USBEth.h>
 *
 *   void setup() {
 *       Serial.begin(115200);
 *       USBEth.begin();
 *       while (!USBEth.connected()) delay(500);
 *       Serial.println(USBEth.localIP());
 *   }
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include "esp_netif.h"
#include "esp_event.h"

#ifdef __cplusplus

/* ── Dongle entry for the known-dongle table ────────────────── */

struct usb_eth_dongle_t {
    uint16_t vid;
    uint16_t pid;
    uint8_t  ecm_config;   /* bConfigurationValue with ECM interfaces */
    const char *name;      /* human-readable, for log messages */
};

/* ── Event callback types ────────────────────────────────────── */

enum usb_eth_event_t {
    USB_ETH_EVENT_CONNECTED,     /* dongle enumerated + ECM link up */
    USB_ETH_EVENT_DISCONNECTED,  /* dongle removed or link down */
    USB_ETH_EVENT_GOT_IP,        /* DHCP lease obtained */
    USB_ETH_EVENT_LOST_IP,       /* IP lost */
};

typedef void (*usb_eth_event_cb_t)(usb_eth_event_t event, void *arg);

/* ── Configuration ───────────────────────────────────────────── */

struct usb_eth_config_t {
    /* I2C VBUS enable (set all to 0 to skip) */
    int      vbus_sda;         /* GPIO for I2C SDA (Tab5: 31) */
    int      vbus_scl;         /* GPIO for I2C SCL (Tab5: 32) */
    uint8_t  vbus_expander_addr; /* I2C address (Tab5: 0x44 for U7) */
    uint8_t  vbus_pin;         /* expander pin (Tab5: 3 for USB5V_EN) */

    /* Optional: second expander pin for EXT5V_EN */
    uint8_t  ext5v_expander_addr; /* (Tab5: 0x43 for U6, or 0 to skip) */
    uint8_t  ext5v_pin;          /* (Tab5: 2 for EXT5V_EN) */

    /* Custom dongle table (NULL = use built-in defaults) */
    const usb_eth_dongle_t *dongles;
    size_t                  dongle_count;
};

/* ── Default config for M5Stack Tab5 ─────────────────────────── */

#define USB_ETH_CONFIG_TAB5() { \
    .vbus_sda = 31, \
    .vbus_scl = 32, \
    .vbus_expander_addr = 0x44, \
    .vbus_pin = 3, \
    .ext5v_expander_addr = 0x43, \
    .ext5v_pin = 2, \
    .dongles = NULL, \
    .dongle_count = 0, \
}

/* ── Main class ──────────────────────────────────────────────── */

class USBEthClass {
public:
    USBEthClass();

    /**
     * Initialize USB Ethernet with default Tab5 config.
     * Enables VBUS, installs USB host stack, starts ECM driver.
     * Returns true on success (does NOT wait for dongle/DHCP).
     */
    bool begin();

    /**
     * Initialize with custom configuration.
     */
    bool begin(const usb_eth_config_t &config);

    /**
     * Tear down USB Ethernet.
     */
    void end();

    /**
     * Returns true if dongle is connected and ECM link is up.
     */
    bool connected() const;

    /**
     * Returns true if we have a DHCP lease.
     */
    bool hasIP() const;

    /**
     * IP address info (valid after GOT_IP event).
     */
    IPAddress localIP() const;
    IPAddress subnetMask() const;
    IPAddress gatewayIP() const;
    IPAddress dnsIP(uint8_t idx = 0) const;

    /**
     * MAC address of the dongle (available after connected()).
     */
    String macAddress() const;

    /**
     * Link speed in Mbps (from ECM notification), 0 if not connected.
     */
    uint32_t linkSpeed() const;

    /**
     * Register event callback.
     */
    void onEvent(usb_eth_event_cb_t cb, void *arg = nullptr);

    /**
     * Get the underlying esp_netif handle for advanced use.
     */
    esp_netif_t *netif() const;

    /**
     * Set static IP (call before begin(), or it uses DHCP).
     */
    void config(IPAddress ip, IPAddress gateway, IPAddress subnet,
                IPAddress dns1 = IPAddress(), IPAddress dns2 = IPAddress());

    /**
     * Set hostname (call before begin()).
     */
    void setHostname(const char *hostname);

private:
    bool _initialized;
    bool _connected;
    bool _has_ip;

    esp_netif_t *_netif;
    usb_eth_config_t _config;

    usb_eth_event_cb_t _event_cb;
    void *_event_cb_arg;

    /* Static IP config (all zero = DHCP) */
    esp_netif_ip_info_t _static_ip;
    bool _use_static_ip;
    const char *_hostname;

    /* Internal helpers */
    bool _enable_vbus();
    bool _install_usb_host();
    bool _start_ecm();
    void _fire_event(usb_eth_event_t event);

    /* ESP event handler (static, routes to instance) */
    static void _ip_event_handler(void *arg, esp_event_base_t base,
                                  int32_t id, void *data);
};

/* ── Global singleton (like WiFi, ETH) ───────────────────────── */

extern USBEthClass USBEth;

#endif /* __cplusplus */
