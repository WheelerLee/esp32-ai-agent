idf_component_register(SRCS "app_ota.c"
                      INCLUDE_DIRS "include"
                      REQUIRES WIFI app_update esp_app_format esp_event esp_http_client esp_partition fatfs json mbedtls nvs_flash wear_levelling)
