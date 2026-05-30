idf_component_register(
    SRCS
       "esp_zb_switch.c"
        "web_server.c"
        "favorites_storage.c"
        "../../../common/zcl_utility/src/zcl_utility.c"
    SRC_DIRS  "." "../../../common/zcl_utility/src"
    INCLUDE_DIRS "." "../../../common/zcl_utility/include"
    REQUIRES nvs_flash esp_driver_gpio esp_driver_uart ieee802154 esp_wifi esp_coex  json esp_event esp_http_server driver esp_driver_usb_serial_jtag esp_coex esp_netif vfs
)