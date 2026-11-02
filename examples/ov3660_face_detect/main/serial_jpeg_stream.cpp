#include "serial_jpeg_stream.hpp"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cstdio>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "img_converters.h"
#include "dl_image_draw.hpp"
#include "driver_behavior.hpp"
#include "driver_alert.hpp"
#include "driver_telemetry.hpp"

namespace {
constexpr uart_port_t kPort = UART_NUM_0;
constexpr int kBaud = 921600;
constexpr size_t kMaxJpeg = 100000; // Matches MAX_JPEG in testiot.py.
DriverBehaviorAnalyzer analyzer;
DriverAlert alert;

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
}

void serial_jpeg_stream_init()
{
    // The viewer scans for the magic bytes, so boot messages sent before this
    // point do not affect framing. Suppress application logs after the switch.
    esp_log_level_set("*", ESP_LOG_NONE);
    const esp_err_t driver_err = uart_driver_install(kPort, 1024, 0, 0, nullptr, 0);
    ESP_ERROR_CHECK(driver_err == ESP_ERR_INVALID_STATE ? ESP_OK : driver_err);
    ESP_ERROR_CHECK(uart_set_baudrate(kPort, kBaud));
    alert.init();
}

void serial_jpeg_stream_send(const who::detect::WhoDetect::result_t &result)
{
    DriverBehavior behavior = analyzer.update(result);
    if (alert.update(behavior)) {
        analyzer.reset_perclos();
    }
    driver_telemetry_update(behavior);
    const auto &img = result.img;
    if (!img.data || img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565BE || img.width != 240 || img.height != 240) {
        return;
    }

    // The capture ring buffer may reuse its frame during JPEG compression.
    // Work on a private copy so a slow UART cannot hold up the camera buffer.
    auto *pixels = static_cast<uint8_t *>(heap_caps_malloc(img.bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels) {
        return;
    }
    std::memcpy(pixels, img.data, img.bytes());

    dl::image::img_t annotated = img;
    annotated.data = pixels;
    for (const auto &face : result.det_res) {
        const int x1 = std::clamp(static_cast<int>(face.box[0]), 0, static_cast<int>(img.width) - 1);
        const int y1 = std::clamp(static_cast<int>(face.box[1]), 0, static_cast<int>(img.height) - 1);
        const int x2 = std::clamp(static_cast<int>(face.box[2]), 0, static_cast<int>(img.width) - 1);
        const int y2 = std::clamp(static_cast<int>(face.box[3]), 0, static_cast<int>(img.height) - 1);
        if (x2 > x1 && y2 > y1) {
            dl::image::draw_hollow_rectangle(annotated, x1, y1, x2, y2, {0xF8, 0x00}, 2);
        }
    }
    if (behavior.face_holdover) {
        const auto &box = behavior.face_box;
        dl::image::draw_hollow_rectangle(annotated, box[0], box[1], box[2], box[3], {0xFF, 0xE0}, 2);
    }

    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    const bool encoded = fmt2jpg(pixels, img.bytes(), img.width, img.height, PIXFORMAT_RGB565, 55, &jpeg, &jpeg_len);
    heap_caps_free(pixels);
    if (!encoded || !jpeg || jpeg_len == 0 || jpeg_len > kMaxJpeg) {
        std::free(jpeg);
        return;
    }

    char status[1152];
    const int status_len = std::snprintf(
        status, sizeof(status),
        "{\"uptime_ms\":%lu,\"driver_present\":%s,\"face_holdover\":%s,"
        "\"scene_luma\":%u,\"face_luma_valid\":%s,\"face_luma\":%u,"
        "\"eyes_valid\":%s,\"eyes_closed\":%s,\"eye_closed_probability\":%.3f,"
        "\"left_eye_aperture\":%.3f,\"right_eye_aperture\":%.3f,"
        "\"left_eye_dark_fraction\":%.3f,\"right_eye_dark_fraction\":%.3f,"
        "\"left_eye_contrast\":%.3f,\"right_eye_contrast\":%.3f,\"eye_closed_duration\":%.2f,"
        "\"perclos_valid\":%s,\"perclos_window_ready\":%s,\"perclos\":%.3f,\"perclos_observed_s\":%.1f,\"blink_count\":%lu,"
        "\"mouth_valid\":%s,\"mouth_open\":%s,\"mouth_open_probability\":%.3f,\"mouth_aperture\":%.3f,"
        "\"mouth_contrast\":%.3f,\"yawn_count\":%lu,"
        "\"blink_window_count\":%lu,\"yawn_window_count\":%lu,\"alert_reasons\":%u,\"buzzer_pattern\":%u,"
        "\"wifi_connected\":%s,\"wifi_disconnect_reason\":%lu,"
        "\"mqtt_connected\":%s,\"mqtt_acked\":%lu,\"mqtt_sequence\":%lu,"
        "\"face_box\":[%d,%d,%d,%d],\"face_keypoints\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
        static_cast<unsigned long>(behavior.uptime_ms),
        behavior.driver_present ? "true" : "false",
        behavior.face_holdover ? "true" : "false",
        static_cast<unsigned>(behavior.scene_luma),
        behavior.face_luma_valid ? "true" : "false",
        static_cast<unsigned>(behavior.face_luma),
        behavior.eyes_valid ? "true" : "false",
        behavior.eyes_closed ? "true" : "false",
        behavior.eye_closed_probability,
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
        driver_telemetry_connected() ? "true" : "false",
        static_cast<unsigned long>(driver_telemetry_acked_count()),
        static_cast<unsigned long>(driver_telemetry_sequence()),
        behavior.face_box[0], behavior.face_box[1], behavior.face_box[2], behavior.face_box[3],
        behavior.face_keypoints[0], behavior.face_keypoints[1], behavior.face_keypoints[2],
        behavior.face_keypoints[3], behavior.face_keypoints[4], behavior.face_keypoints[5],
        behavior.face_keypoints[6], behavior.face_keypoints[7], behavior.face_keypoints[8],
        behavior.face_keypoints[9]);
    if (status_len > 0 && status_len < static_cast<int>(sizeof(status))) {
        write_packet(0x56, status, status_len);
    }
    write_packet(0x55, jpeg, jpeg_len);
    std::free(jpeg);
}
