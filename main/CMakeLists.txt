idf_component_register(
    SRCS "main.c" "bbs.c" "bbs_storage.c" "bbs_heard.c" "sysop_auth.c"
    INCLUDE_DIRS "."
    REQUIRES esp-ax25 esp_wifi esp_event esp_netif nvs_flash littlefs
)
