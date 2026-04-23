idf_component_register(
    SRCS "src/USBEth.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_event
        esp_netif
        usb
        iot_usbh_ecm
        iot_usbh_cdc
        iot_eth
)
