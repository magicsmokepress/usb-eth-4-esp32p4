/*
 * USBEth Basic Example — USB Ethernet on M5Stack Tab5
 *
 * Connects a class-compliant CDC-ECM dongle, gets DHCP,
 * prints IP, and makes an HTTP request to prove data flows.
 *
 * Build: idf.py build flash monitor
 *
 * Note: On ESP32-P4, Arduino's Serial object goes to UART0, not the
 * USB Serial JTAG used by idf.py monitor. We use printf() + ESP_LOG
 * so output goes to the same console as IDF log messages.
 */

#include <Arduino.h>
#include <USBEth.h>

extern "C" {
#include "esp_log.h"
#include "esp_http_client.h"
}

static const char *TAG = "main";

/* ── Event callback (optional) ───────────────────────────────── */

void onEthEvent(usb_eth_event_t event, void *arg) {
    switch (event) {
        case USB_ETH_EVENT_GOT_IP:
            ESP_LOGI(TAG, "[ETH] Got IP address");
            break;
        case USB_ETH_EVENT_LOST_IP:
            ESP_LOGW(TAG, "[ETH] Lost IP address");
            break;
        default:
            break;
    }
}

/* ── Arduino setup/loop ──────────────────────────────────────── */

void setup() {
    delay(2000);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  USBEth Basic — USB Ethernet on Tab5   ");
    ESP_LOGI(TAG, "========================================");

    /* Register event callback */
    USBEth.onEvent(onEthEvent);

    /* Optional: set hostname before begin() */
    USBEth.setHostname("tab5-eth");

    /* Start USB Ethernet (default Tab5 config) */
    if (!USBEth.begin()) {
        ESP_LOGE(TAG, "USBEth.begin() FAILED");
        while (1) delay(1000);
    }

    ESP_LOGI(TAG, "Waiting for dongle + DHCP...");

    /* Wait for IP (timeout after 30 seconds) */
    uint32_t start = millis();
    while (!USBEth.hasIP()) {
        if (millis() - start > 30000) {
            ESP_LOGE(TAG, "Timeout waiting for IP. Check dongle + cable.");
            while (1) delay(1000);
        }
        delay(500);
        printf(".");
        fflush(stdout);
    }
    printf("\n");

    /* Print connection info */
    ESP_LOGI(TAG, "--- Connected ---");
    ESP_LOGI(TAG, "  IP:      %s", USBEth.localIP().toString().c_str());
    ESP_LOGI(TAG, "  Mask:    %s", USBEth.subnetMask().toString().c_str());
    ESP_LOGI(TAG, "  Gateway: %s", USBEth.gatewayIP().toString().c_str());
    ESP_LOGI(TAG, "  DNS:     %s", USBEth.dnsIP().toString().c_str());
    ESP_LOGI(TAG, "  MAC:     %s", USBEth.macAddress().c_str());
    ESP_LOGI(TAG, "-----------------");

    /* Quick HTTP test to prove data flows */
    ESP_LOGI(TAG, "Fetching http://httpbin.org/ip ...");

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = "http://httpbin.org/ip";
    http_cfg.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            int content_length = esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);

            if (content_length > 0 && content_length < 1024) {
                char *buf = (char *)malloc(content_length + 1);
                if (buf) {
                    int read_len = esp_http_client_read(client, buf, content_length);
                    buf[read_len] = '\0';
                    ESP_LOGI(TAG, "HTTP %d: %s", status, buf);
                    free(buf);
                }
            } else {
                ESP_LOGI(TAG, "HTTP %d (content-length: %d)", status, content_length);
            }
            esp_http_client_close(client);
        } else {
            ESP_LOGE(TAG, "HTTP connect failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }

    ESP_LOGI(TAG, "Done. USB Ethernet is running.");
}

void loop() {
    delay(10000);

    if (USBEth.hasIP()) {
        ESP_LOGI(TAG, "[%lu] IP: %s", millis() / 1000,
                 USBEth.localIP().toString().c_str());
    } else {
        ESP_LOGW(TAG, "[%lu] No IP", millis() / 1000);
    }
}
