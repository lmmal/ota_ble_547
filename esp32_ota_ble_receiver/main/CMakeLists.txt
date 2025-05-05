idf_component_register(
    SRCS
        "main.c"
        "ota_ble_receiver.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash app_update efuse mbedtls
)

