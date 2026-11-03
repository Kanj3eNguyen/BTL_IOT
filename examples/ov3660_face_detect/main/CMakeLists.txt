set(include_dirs    ./)

set(requires who_detect_app espressif__esp32-camera espressif__esp_new_jpeg esp_driver_uart esp_driver_gpio esp_driver_rmt esp_wifi esp_netif esp_event nvs_flash mqtt mbedtls json)

set(driver_ca "certs/tb-cloud-root-ca.pem")
if(CONFIG_DRIVER_MQTT_GUARD_ENABLED)
    set(driver_ca "certs/ingest-root-ca.pem")
endif()

set(detect_component ${DETECT_MODEL} espressif__${DETECT_MODEL})

idf_component_register(
    SRCS app_main.cpp frame_cap_pipeline.cpp serial_jpeg_stream.cpp driver_behavior.cpp driver_alert.cpp driver_status_led.cpp driver_telemetry.cpp driver_lighting.cpp driver_message_auth.cpp
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires}
    EMBED_TXTFILES ${driver_ca}
)
idf_component_optional_requires(PUBLIC ${detect_component})
