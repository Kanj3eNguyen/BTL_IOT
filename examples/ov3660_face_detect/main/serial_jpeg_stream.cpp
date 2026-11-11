#include "serial_jpeg_stream.hpp"

#include <cstring>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_jpeg_enc.h"
#include "dl_image_draw.hpp"
#include "driver_behavior.hpp"
#include "driver_frame_guard.hpp"
#include "driver_alert.hpp"
#include "driver_lighting.hpp"
#include "driver_telemetry.hpp"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr uart_port_t kPort = UART_NUM_0;
constexpr int kBaud = 1500000;
constexpr size_t kMaxJpeg = 100000;
constexpr size_t kFrameBytes = 240 * 240 * 2;
constexpr uint8_t kSlots = 3;
DriverBehaviorAnalyzer analyzer;
DriverAlert alert;
struct StreamFrame {
    uint8_t *pixels = nullptr;
    dl::image::img_t image{};
    DriverBehavior behavior{};
    int64_t capture_us = 0;
    float analysis_hz = 0;
};
StreamFrame slots[kSlots];
QueueHandle_t free_frames = nullptr;
QueueHandle_t pending_frame = nullptr;
std::atomic_uint32_t dropped{0};
std::atomic_uint32_t camera_bad_frames{0};
std::atomic_uint32_t rejected_quality{0};
uint8_t *jpeg = nullptr;

uint32_t packet_checksum(const uint8_t *data, size_t length, uint32_t hash = 2166136261u)
{
    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ data[i]) * 16777619u;
    }
    return hash;
}

void write_packet(uint8_t kind, const void *payload, size_t length)
{
    const uint32_t n = static_cast<uint32_t>(length);
    const uint8_t header[8] = {
        0xAA, 0x55, 0xAA, kind,
        static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8),
        static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 24),
    };
    uint32_t checksum = packet_checksum(header + 3, sizeof(header) - 3);
    checksum = packet_checksum(static_cast<const uint8_t *>(payload), length, checksum);
    const uint8_t trailer[4] = {
        static_cast<uint8_t>(checksum),
        static_cast<uint8_t>(checksum >> 8),
        static_cast<uint8_t>(checksum >> 16),
        static_cast<uint8_t>(checksum >> 24),
    };
    uart_write_bytes(kPort, header, sizeof(header));
    uart_write_bytes(kPort, payload, length);
    uart_write_bytes(kPort, trailer, sizeof(trailer));
}

void stream_task(void *)
{
    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = 240;
    config.height = 240;
    config.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    config.subsampling = JPEG_SUBSAMPLE_420;
    config.quality = 50;
    config.task_enable = false;
    jpeg_enc_handle_t encoder = nullptr;
    ESP_ERROR_CHECK(jpeg_enc_open(&config, &encoder) == JPEG_ERR_OK ? ESP_OK : ESP_FAIL);
    while (true) {
        uint8_t index = 0;
        xQueueReceive(pending_frame, &index, portMAX_DELAY);
        auto &slot = slots[index];
        const auto &behavior = slot.behavior;
        const auto &box = behavior.face_box;
        if (behavior.driver_present && box[2] > box[0] && box[3] > box[1]) {
            dl::image::draw_hollow_rectangle(slot.image,
                std::clamp(box[0], 0, 239), std::clamp(box[1], 0, 239),
                std::clamp(box[2], 0, 239), std::clamp(box[3], 0, 239),
                behavior.face_holdover ? std::vector<uint8_t>{0xFF, 0xE0}
                                       : std::vector<uint8_t>{0xF8, 0x00}, 2);
        }
        const int64_t encode_start = esp_timer_get_time();
        int jpeg_len = 0;
        const auto encoded = jpeg_enc_process(encoder, slot.pixels, kFrameBytes,
                                              jpeg, kMaxJpeg, &jpeg_len);
        const float encode_ms = (esp_timer_get_time() - encode_start) / 1000.0f;
        const float frame_age_ms = std::max<int64_t>(0, esp_timer_get_time() - slot.capture_us) / 1000.0f;
        if (encoded != JPEG_ERR_OK || jpeg_len <= 0 || jpeg_len > static_cast<int>(kMaxJpeg)) {
            dropped.fetch_add(1);
            xQueueSend(free_frames, &index, 0);
            continue;
        }
        char status[2048];
        const int status_len = std::snprintf(
            status, sizeof(status),
            "{\"analysis_hz\":%.1f,\"encode_ms\":%.1f,\"frame_age_ms\":%.1f,\"stream_dropped\":%lu,"
            "\"camera_bad_frames\":%lu,\"camera_bad_delta\":%lu,\"camera_bad_swapped_delta\":%lu,"
            "\"auto_light\":%s,\"ae_level\":%d,\"lighting_state\":%u,\"uptime_ms\":%lu,\"driver_present\":%s,\"face_holdover\":%s,"
            "\"scene_luma\":%u,\"face_luma_valid\":%s,\"face_luma\":%u,"
            "\"eyes_valid\":%s,\"eyes_closed\":%s,\"eye_closed_probability\":%.3f,"
            "\"left_eye_closed_probability\":%.3f,\"right_eye_closed_probability\":%.3f,"
            "\"left_eye_aperture\":%.3f,\"right_eye_aperture\":%.3f,"
            "\"left_eye_dark_fraction\":%.3f,\"right_eye_dark_fraction\":%.3f,"
            "\"left_eye_contrast\":%.3f,\"right_eye_contrast\":%.3f,\"eye_closed_duration\":%.2f,"
            "\"perclos_valid\":%s,\"perclos_window_ready\":%s,\"perclos\":%.3f,\"perclos_observed_s\":%.1f,\"blink_count\":%lu,"
            "\"mouth_valid\":%s,\"mouth_open\":%s,\"mouth_open_probability\":%.3f,\"mouth_aperture\":%.3f,"
            "\"mouth_contrast\":%.3f,\"yawn_count\":%lu,"
            "\"blink_window_count\":%lu,\"yawn_window_count\":%lu,\"alert_reasons\":%u,\"buzzer_pattern\":%u,"
            "\"wifi_connected\":%s,\"wifi_disconnect_reason\":%lu,\"time_synced\":%s,"
            "\"mqtt_connected\":%s,\"mqtt_acked\":%lu,\"mqtt_sequence\":%lu,"
            "\"guard_enabled\":%s,\"guard_ready\":%s,\"guard_forwarded_sequence\":%lu,\"guard_forwarded_count\":%lu,"
            "\"tls_error\":%d,\"tls_stack_error\":%d,\"heap_internal\":%u,\"heap_largest\":%u,"
            "\"face_box\":[%d,%d,%d,%d],\"face_keypoints\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
            slot.analysis_hz, encode_ms, frame_age_ms,
            static_cast<unsigned long>(dropped.load()),
            static_cast<unsigned long>(camera_bad_frames.load()),
            static_cast<unsigned long>(rejected_quality.load() >> 16),
            static_cast<unsigned long>(rejected_quality.load() & 0xffff),
            behavior.auto_light ? "true" : "false", static_cast<int>(behavior.ae_level),
            static_cast<unsigned>(behavior.lighting_state),
            static_cast<unsigned long>(behavior.uptime_ms),
            behavior.driver_present ? "true" : "false",
            behavior.face_holdover ? "true" : "false",
            static_cast<unsigned>(behavior.scene_luma),
            behavior.face_luma_valid ? "true" : "false",
            static_cast<unsigned>(behavior.face_luma),
            behavior.eyes_valid ? "true" : "false",
            behavior.eyes_closed ? "true" : "false",
            behavior.eye_closed_probability,
            behavior.left_eye_closed_probability, behavior.right_eye_closed_probability,
            behavior.left_eye_aperture, behavior.right_eye_aperture,
            behavior.left_eye_dark_fraction, behavior.right_eye_dark_fraction,
            behavior.left_eye_contrast, behavior.right_eye_contrast,
            behavior.eye_closed_duration_s,
            behavior.perclos_valid ? "true" : "false",
            behavior.perclos_window_ready ? "true" : "false", behavior.perclos,
            behavior.perclos_observed_s,
            static_cast<unsigned long>(behavior.blink_count),
            behavior.mouth_valid ? "true" : "false", behavior.mouth_open ? "true" : "false",
            behavior.mouth_open_probability,
            behavior.mouth_aperture, behavior.mouth_contrast,
            static_cast<unsigned long>(behavior.yawn_count),
            static_cast<unsigned long>(behavior.blink_window_count),
            static_cast<unsigned long>(behavior.yawn_window_count),
            static_cast<unsigned>(behavior.alert_reasons),
            static_cast<unsigned>(behavior.buzzer_pattern),
            driver_telemetry_wifi_connected() ? "true" : "false",
            static_cast<unsigned long>(driver_telemetry_wifi_disconnect_reason()),
            driver_telemetry_time_synced() ? "true" : "false",
            driver_telemetry_connected() ? "true" : "false",
            static_cast<unsigned long>(driver_telemetry_acked_count()),
            static_cast<unsigned long>(driver_telemetry_sequence()),
            driver_telemetry_guard_enabled() ? "true" : "false",
            driver_telemetry_guard_ready() ? "true" : "false",
            static_cast<unsigned long>(driver_telemetry_guard_forwarded_sequence()),
            static_cast<unsigned long>(driver_telemetry_guard_forwarded_count()),
            driver_telemetry_tls_error(), driver_telemetry_tls_stack_error(),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            behavior.face_box[0], behavior.face_box[1], behavior.face_box[2], behavior.face_box[3],
            behavior.face_keypoints[0], behavior.face_keypoints[1], behavior.face_keypoints[2],
            behavior.face_keypoints[3], behavior.face_keypoints[4], behavior.face_keypoints[5],
            behavior.face_keypoints[6], behavior.face_keypoints[7], behavior.face_keypoints[8],
            behavior.face_keypoints[9]);
        if (status_len > 0 && status_len < static_cast<int>(sizeof(status))) {
            write_packet(0x56, status, status_len);
        }
        write_packet(0x55, jpeg, jpeg_len);

        // Keep at most one frame on the wire; never build a multi-frame TX backlog.
        uart_wait_tx_done(kPort, pdMS_TO_TICKS(300));
        xQueueSend(free_frames, &index, 0);
    }
}
} // namespace

void serial_jpeg_stream_init()
{
    esp_log_level_set("*", ESP_LOG_NONE);
    const esp_err_t driver_err = uart_driver_install(kPort, 1024, 8192, 0, nullptr, 0);
    ESP_ERROR_CHECK(driver_err == ESP_ERR_INVALID_STATE ? ESP_OK : driver_err);
    ESP_ERROR_CHECK(uart_set_baudrate(kPort, kBaud));
    free_frames = xQueueCreate(kSlots, sizeof(uint8_t));
    pending_frame = xQueueCreate(1, sizeof(uint8_t));
    configASSERT(free_frames && pending_frame);
    for (uint8_t i = 0; i < kSlots; ++i) {
        slots[i].pixels = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        configASSERT(slots[i].pixels);
        xQueueSend(free_frames, &i, 0);
    }
    jpeg = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16, kMaxJpeg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    configASSERT(jpeg);
    alert.init();
    configASSERT(xTaskCreatePinnedToCore(stream_task, "serial_preview", 8192,
                                         nullptr, 2, nullptr, 0) == pdPASS);
}

bool serial_jpeg_stream_validate_frame(const dl::image::img_t &image)
{
    if (image.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565BE || image.width != 240 || image.height != 240) {
        return false;
    }
    const auto quality = driver_frame_quality(static_cast<const uint8_t *>(image.data), image.width, image.height);
    if (!quality.valid) {
        camera_bad_frames.fetch_add(1);
        rejected_quality.store((quality.normal_delta << 16) | quality.swapped_delta);
    }
    return quality.valid;
}

void serial_jpeg_stream_send(const who::detect::WhoDetect::result_t &result)
{
    DriverBehavior behavior = analyzer.update(result);
    driver_lighting_update(behavior);
    if (alert.update(behavior)) {
        analyzer.reset_perclos();
    }
    driver_telemetry_update(behavior);
    const auto &img = result.img;
    if (!img.data || img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565BE ||
        img.width != 240 || img.height != 240) {
        return;
    }
    static int64_t previous = 0;
    static float analysis_hz = 0;
    const int64_t now = esp_timer_get_time();
    if (previous && now > previous) {
        const float hz = 1000000.0f / (now - previous);
        analysis_hz = analysis_hz ? analysis_hz * 0.85f + hz * 0.15f : hz;
    }
    previous = now;
    uint8_t index = 0;
    if (xQueueReceive(free_frames, &index, 0) != pdTRUE) {
        dropped.fetch_add(1);
        return;
    }
    auto &slot = slots[index];
    std::memcpy(slot.pixels, img.data, kFrameBytes);
    slot.image = img;
    slot.image.data = slot.pixels;
    slot.behavior = behavior;
    slot.capture_us = result.timestamp;
    slot.analysis_hz = analysis_hz;
    uint8_t stale = 0;
    if (xQueueReceive(pending_frame, &stale, 0) == pdTRUE) {
        xQueueSend(free_frames, &stale, 0);
        dropped.fetch_add(1);
    }
    if (xQueueSend(pending_frame, &index, 0) != pdTRUE) {
        xQueueSend(free_frames, &index, 0);
        dropped.fetch_add(1);
    }
}
