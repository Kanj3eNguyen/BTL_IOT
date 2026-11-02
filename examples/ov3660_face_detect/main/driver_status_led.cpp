#include "driver_status_led.hpp"

#include <cstdint>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "DRIVER_LED";
constexpr gpio_num_t kLedPin = GPIO_NUM_48;
constexpr uint32_t kResolutionHz = 10000000;
constexpr uint8_t kGreenBrightness = 24;
constexpr uint32_t kFlashMs = 150;

rmt_channel_handle_t channel = nullptr;
rmt_encoder_handle_t encoder = nullptr;
TaskHandle_t led_task_handle = nullptr;

bool write_pixel(uint8_t green)
{
    // WS2812 expects G, R, B in that order, most significant bit first.
    const uint8_t pixel[3] = {green, 0, 0};
    const rmt_transmit_config_t transmit_config = {};
    esp_err_t err = rmt_transmit(channel, encoder, pixel, sizeof(pixel), &transmit_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(channel, 100);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "GPIO48 LED update failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void led_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        write_pixel(kGreenBrightness);
        vTaskDelay(pdMS_TO_TICKS(kFlashMs));
        write_pixel(0);
    }
}
} // namespace

void driver_status_led_start()
{
    rmt_tx_channel_config_t channel_config = {};
    channel_config.gpio_num = kLedPin;
    channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
    channel_config.resolution_hz = kResolutionHz;
    channel_config.mem_block_symbols = 64;
    channel_config.trans_queue_depth = 2;
    esp_err_t err = rmt_new_tx_channel(&channel_config, &channel);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "GPIO48 LED unavailable: %s", esp_err_to_name(err));
        return;
    }

    rmt_bytes_encoder_config_t encoder_config = {};
    encoder_config.bit0.level0 = 1;
    encoder_config.bit0.duration0 = 3;
    encoder_config.bit0.level1 = 0;
    encoder_config.bit0.duration1 = 9;
    encoder_config.bit1.level0 = 1;
    encoder_config.bit1.duration0 = 9;
    encoder_config.bit1.level1 = 0;
    encoder_config.bit1.duration1 = 3;
    encoder_config.flags.msb_first = 1;
    err = rmt_new_bytes_encoder(&encoder_config, &encoder);
    if (err == ESP_OK) {
        err = rmt_enable(channel);
    }
    if (err == ESP_OK) {
        // Clear any color latched by an earlier firmware or a software reset.
        write_pixel(0);
        if (xTaskCreate(led_task, "mqtt_led", 2048, nullptr, 2, &led_task_handle) == pdPASS) {
            return;
        }
        err = ESP_ERR_NO_MEM;
    }
    ESP_LOGW(kTag, "GPIO48 LED setup failed: %s", esp_err_to_name(err));
    if (encoder) {
        rmt_del_encoder(encoder);
        encoder = nullptr;
    }
    if (channel) {
        rmt_disable(channel);
        rmt_del_channel(channel);
        channel = nullptr;
    }
}

void driver_status_led_mqtt_published()
{
    if (led_task_handle) {
        xTaskNotifyGive(led_task_handle);
    }
}
