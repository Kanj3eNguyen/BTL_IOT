set(include_dirs    ./)

set(requires who_detect_app espressif__esp32-camera esp_driver_uart esp_driver_gpio esp_driver_rmt esp_wifi esp_netif esp_event nvs_flash mqtt)

set(detect_component ${DETECT_MODEL} espressif__${DETECT_MODEL})

idf_component_register(
    SRCS app_main.cpp frame_cap_pipeline.cpp serial_jpeg_stream.cpp driver_behavior.cpp driver_alert.cpp driver_status_led.cpp driver_telemetry.cpp
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires}
)
idf_component_optional_requires(PUBLIC ${detect_component})
