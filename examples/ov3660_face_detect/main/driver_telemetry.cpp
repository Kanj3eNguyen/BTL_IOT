#include "driver_telemetry.hpp"
#include "driver_alert.hpp"
#include "driver_status_led.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr char kTopic[] = "v1/devices/me/telemetry";
constexpr char kTag[] = "DRIVER_MQTT";
QueueHandle_t latest_behavior = nullptr;
esp_mqtt_client_handle_t mqtt = nullptr;
std::atomic_bool mqtt_connected{false};
std::atomic_bool wifi_connected{false};
std::atomic_uint32_t wifi_disconnect_reason{0};
std::atomic_uint32_t mqtt_acked_count{0};
std::atomic_uint32_t mqtt_sequence{0};
uint32_t boot_id = 0;

void format_alert_reasons(uint8_t reasons, char (&text)[128])
{
    text[0] = '\0';
    const auto append = [&text](const char *reason) {
        const size_t used = std::strlen(text);
        std::snprintf(text + used, sizeof(text) - used, "%s%s", used ? ", " : "", reason);
    };
    if (reasons & DRIVER_ALERT_EYES_CLOSED) append("Nhắm mắt lâu");
    if (reasons & DRIVER_ALERT_YAWN_RATE) append("Ngáp nhiều");
    if (reasons & DRIVER_ALERT_PERCLOS) append("PERCLOS cao");
    if (reasons & DRIVER_ALERT_NO_FACE) append("Không thấy mặt");
    if (!text[0]) append("Bình thường");
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *event_data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_connected.store(true);
        wifi_disconnect_reason.store(0);
        return;
    }
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const auto *disconnected = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
            wifi_connected.store(false);
            wifi_disconnect_reason.store(disconnected ? disconnected->reason : 0);
            mqtt_connected.store(false);
        }
        esp_wifi_connect();
    }
}

void on_mqtt_event(void *, esp_event_base_t, int32_t id, void *)
{
    if (id == MQTT_EVENT_CONNECTED) {
        mqtt_connected.store(true);
    } else if (id == MQTT_EVENT_PUBLISHED) {
        mqtt_acked_count.fetch_add(1);
        driver_status_led_mqtt_published();
    } else if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR) {
        mqtt_connected.store(false);
    }
}

void publish_task(void *)
{
    DriverBehavior behavior{};
    bool have_sample = false;
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONFIG_DRIVER_MQTT_INTERVAL_MS));
        DriverBehavior newest;
        if (xQueueReceive(latest_behavior, &newest, 0) == pdTRUE) {
            behavior = newest;
            have_sample = true;
        }
        if (!have_sample) {
            continue;
        }
        // Sequence advances on every scheduled sample, including offline
        // intervals. Gaps on ThingsBoard expose skipped send opportunities.
        const uint32_t sequence = mqtt_sequence.fetch_add(1) + 1;
        if (!mqtt_connected.load()) {
            continue;
        }

        // Null values distinguish unavailable observations from false/zero.
        char eyes[8], mouth[8], perclos[20], eye_probability[20], mouth_probability[20], face_luma[8];
        std::snprintf(eyes, sizeof(eyes), "%s", behavior.eyes_closed ? "true" : "false");
        std::snprintf(mouth, sizeof(mouth), "%s", behavior.mouth_open ? "true" : "false");
        std::snprintf(perclos, sizeof(perclos), "%.3f", behavior.perclos);
        std::snprintf(eye_probability, sizeof(eye_probability), "%.3f", behavior.eye_closed_probability);
        std::snprintf(mouth_probability, sizeof(mouth_probability), "%.3f", behavior.mouth_open_probability);
        std::snprintf(face_luma, sizeof(face_luma), "%u", static_cast<unsigned>(behavior.face_luma));
        char alert_reasons_text[128];
        format_alert_reasons(behavior.alert_reasons, alert_reasons_text);
        char payload[768];
        const int size = std::snprintf(
            payload, sizeof(payload),
            "{\"boot_id\":%lu,\"sequence\":%lu,"
            "\"uptime_ms\":%lu,\"driver_present\":%s,\"face_holdover\":%s,"
            "\"scene_luma\":%u,\"face_luma\":%s,"
            "\"eyes_valid\":%s,\"eyes_closed\":%s,\"eye_closed_probability\":%s,"
            "\"blink_count\":%lu,\"eye_closed_duration\":%.2f,"
            "\"mouth_valid\":%s,\"mouth_open\":%s,\"mouth_open_probability\":%s,"
            "\"yawn_count\":%lu,\"blink_window_count\":%lu,"
            "\"yawn_window_count\":%lu,\"alert_reasons\":\"%s\","
            "\"alert_reasons_mask\":%u,\"buzzer_pattern\":%u,"
            "\"alert_active\":%s,\"perclos_valid\":%s,\"perclos_window_ready\":%s,"
            "\"perclos\":%s,\"perclos_observed_s\":%.1f}",
            static_cast<unsigned long>(boot_id), static_cast<unsigned long>(sequence),
            static_cast<unsigned long>(behavior.uptime_ms),
            behavior.driver_present ? "true" : "false",
            behavior.face_holdover ? "true" : "false",
            static_cast<unsigned>(behavior.scene_luma),
            behavior.face_luma_valid ? face_luma : "null",
            behavior.eyes_valid ? "true" : "false",
            behavior.eyes_valid ? eyes : "null",
            behavior.eyes_valid ? eye_probability : "null",
            static_cast<unsigned long>(behavior.blink_count), behavior.eye_closed_duration_s,
            behavior.mouth_valid ? "true" : "false",
            behavior.mouth_valid ? mouth : "null",
            behavior.mouth_valid ? mouth_probability : "null",
            static_cast<unsigned long>(behavior.yawn_count),
            static_cast<unsigned long>(behavior.blink_window_count),
            static_cast<unsigned long>(behavior.yawn_window_count),
            alert_reasons_text,
            static_cast<unsigned>(behavior.alert_reasons),
            static_cast<unsigned>(behavior.buzzer_pattern),
            behavior.alert_reasons ? "true" : "false",
            behavior.perclos_valid ? "true" : "false",
            behavior.perclos_window_ready ? "true" : "false",
            behavior.perclos_valid ? perclos : "null",
            behavior.perclos_observed_s);
        if (size > 0 && size < static_cast<int>(sizeof(payload))) {
            // Enqueue avoids blocking face detection; send at QoS 1.
            esp_mqtt_client_enqueue(mqtt, kTopic, payload, size, 1, 0, false);
        }
    }
}
} // namespace

void driver_telemetry_start()
{
    if (CONFIG_DRIVER_WIFI_SSID[0] == '\0' || CONFIG_DRIVER_MQTT_TOKEN[0] == '\0' ||
        CONFIG_DRIVER_MQTT_URI[0] == '\0') {
        ESP_LOGW(kTag, "Wi-Fi/MQTT settings missing; telemetry disabled");
        return;
    }
    if (std::strncmp(CONFIG_DRIVER_MQTT_URI, "mqtt://", 7) != 0) {
        ESP_LOGE(kTag, "Use an mqtt:// URI for this build");
        return;
    }
    if (std::strlen(CONFIG_DRIVER_WIFI_SSID) > 32 || std::strlen(CONFIG_DRIVER_WIFI_PASSWORD) > 64) {
        ESP_LOGE(kTag, "Wi-Fi credentials exceed ESP32 limits");
        return;
    }
    const esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }
    latest_behavior = xQueueCreate(1, sizeof(DriverBehavior));
    ESP_ERROR_CHECK(latest_behavior ? ESP_OK : ESP_ERR_NO_MEM);
    boot_id = esp_random();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi = {};
    std::memcpy(wifi.sta.ssid, CONFIG_DRIVER_WIFI_SSID, std::strlen(CONFIG_DRIVER_WIFI_SSID));
    std::memcpy(wifi.sta.password, CONFIG_DRIVER_WIFI_PASSWORD, std::strlen(CONFIG_DRIVER_WIFI_PASSWORD));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));

    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = CONFIG_DRIVER_MQTT_URI;
    config.credentials.username = CONFIG_DRIVER_MQTT_TOKEN;
    mqtt = esp_mqtt_client_init(&config);
    ESP_ERROR_CHECK(mqtt ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, on_mqtt_event, nullptr));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
    ESP_ERROR_CHECK(xTaskCreate(publish_task, "driver_mqtt", 4096, nullptr, 3, nullptr) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_wifi_start());
}

void driver_telemetry_update(const DriverBehavior &behavior)
{
    if (latest_behavior) {
        xQueueOverwrite(latest_behavior, &behavior);
    }
}

bool driver_telemetry_connected()
{
    return mqtt_connected.load();
}

bool driver_telemetry_wifi_connected()
{
    return wifi_connected.load();
}

uint32_t driver_telemetry_wifi_disconnect_reason()
{
    return wifi_disconnect_reason.load();
}

uint32_t driver_telemetry_acked_count()
{
    return mqtt_acked_count.load();
}

uint32_t driver_telemetry_sequence()
{
    return mqtt_sequence.load();
}
