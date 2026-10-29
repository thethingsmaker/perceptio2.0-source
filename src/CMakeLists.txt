idf_component_register(
    SRCS "main.c" "board.c" "bq25180.c" "lp5810.c" "led.c" "clock.c" "cloud.c" "imu.c" "w25n01kv.c" "power.c" "net.c" "sampler.c" "store.c" "pcr2_codec1.c" "upload.c" "ota.c" "bus.c" "app.c" "rlog.c" "selftest.c" "diag.c" "button.c" "netwatch.c" "led_policy.c" "power_mgr.c" "config.c" "webui.c" "web_index.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES driver esp_adc esp_wifi esp_netif esp_event nvs_flash esp_timer esp_psram spi_flash esp_http_server esp_http_client json lwip mqtt mbedtls esp-tls espcoredump esp_https_ota app_update lsm6dsv320x
)
