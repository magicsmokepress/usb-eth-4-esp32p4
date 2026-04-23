/*
 * BasicEthernet.ino — USB Ethernet on M5Stack Tab5
 *
 * Plug in a class-compliant USB Ethernet dongle.
 * Wait for DHCP. Print your IP. Done.
 *
 * Requires: Custom Arduino core built with
 *   CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y
 * See: arduino/lib-builder-patch/README.md
 */

#include <USBEth.h>

void onEthEvent(usb_eth_event_t event, void *arg) {
    switch (event) {
        case USB_ETH_EVENT_GOT_IP:
            Serial.println("[ETH] Got IP!");
            break;
        case USB_ETH_EVENT_LOST_IP:
            Serial.println("[ETH] Lost IP");
            break;
        default:
            break;
    }
}

void setup() {
    delay(2000);
    Serial.begin(115200);
    Serial.println("\n=== USBEth Basic ===\n");

    USBEth.onEvent(onEthEvent);
    USBEth.setHostname("tab5-eth");

    if (!USBEth.begin()) {
        Serial.println("USBEth.begin() FAILED");
        while (1) delay(1000);
    }

    Serial.println("Waiting for dongle + DHCP...");
    uint32_t start = millis();
    while (!USBEth.hasIP()) {
        if (millis() - start > 30000) {
            Serial.println("Timeout. Check dongle + cable.");
            while (1) delay(1000);
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    Serial.println("--- Connected ---");
    Serial.printf("  IP:      %s\n", USBEth.localIP().toString().c_str());
    Serial.printf("  Gateway: %s\n", USBEth.gatewayIP().toString().c_str());
    Serial.printf("  MAC:     %s\n", USBEth.macAddress().c_str());
    Serial.println("-----------------\n");

    Serial.println("USB Ethernet is running.");
}

void loop() {
    delay(10000);
    if (USBEth.hasIP()) {
        Serial.printf("[%lu] IP: %s\n", millis() / 1000,
                      USBEth.localIP().toString().c_str());
    }
}
