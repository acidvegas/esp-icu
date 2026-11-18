idf_component_register(
    SRCS "main.c" "settings.c" "camera.c" "motion.c" "recorder.c" "storage.c" "web.c" "notify.c" "audio.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_timer fatfs sdmmc esp_driver_sdmmc esp_driver_gpio mbedtls lwip esp_http_client esp_driver_tsens esp_driver_i2s
)
