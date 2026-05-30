idf_component_register(
    SRCS
       
       
        "favorites_storage.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_driver_gpio
        esp_driver_uart
        ieee802154
        esp_wifi
        json
        esp_event
        esp_http_server
        driver
        esp_driver_usb_serial_jtag
        esp_coex
        esp_netif
        vfs
        zcl_utility
)