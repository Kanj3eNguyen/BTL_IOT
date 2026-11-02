#include "driver_alert.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
template <size_t N>
uint32_t count_recent(const std::array<uint64_t, N> &events, uint64_t now, uint64_t window_us)
{
    uint32_t count = 0;
    for (uint64_t time : events) {
        if (time && now >= time && now - time < window_us) {
            ++count;
        }
    }
    return count;
}

constexpr uint64_t kYawnBurstDurationUs = 10000000;
constexpr uint64_t kPerclosBurstDurationUs = 15000000;
#if CONFIG_DRIVER_BUZZER_ENABLED
constexpr uint32_t kBurstPeriodMs = 2000;
constexpr uint32_t kNoFacePeriodMs = 3000;

uint8_t active_pattern(uint64_t now, uint8_t reasons, bool driver_present,
                       uint64_t yawn_start_us, uint64_t perclos_start_us)
{
    if (reasons & DRIVER_ALERT_EYES_CLOSED) {
        return DRIVER_BUZZER_EYES;
    }
    if (!driver_present) {
        return reasons & DRIVER_ALERT_NO_FACE ? DRIVER_BUZZER_NO_FACE_SLOW : DRIVER_BUZZER_OFF;
    }
    if (perclos_start_us && now >= perclos_start_us &&
        now - perclos_start_us < kPerclosBurstDurationUs) {
        return DRIVER_BUZZER_PERCLOS_TRIPLE;
    }
    if (yawn_start_us && now >= yawn_start_us &&
        now - yawn_start_us < kYawnBurstDurationUs) {
        return DRIVER_BUZZER_YAWN_DOUBLE;
    }
    return DRIVER_BUZZER_OFF;
}

bool burst_pulse(uint64_t now, uint64_t start_us, uint8_t pulses)
{
    const uint32_t phase_ms = static_cast<uint32_t>((now - start_us) / 1000 % kBurstPeriodMs);
    for (uint8_t i = 0; i < pulses; ++i) {
        const uint32_t pulse_start_ms = static_cast<uint32_t>(i) * 240;
        if (phase_ms >= pulse_start_ms && phase_ms < pulse_start_ms + 120) {
            return true;
        }
    }
    return false;
}

constexpr gpio_num_t kBuzzerPin = static_cast<gpio_num_t>(CONFIG_DRIVER_BUZZER_GPIO);
#ifdef CONFIG_DRIVER_BUZZER_ACTIVE_LOW
constexpr int kInactiveLevel = 1;
#else
constexpr int kInactiveLevel = 0;
#endif
constexpr int kActiveLevel = 1 - kInactiveLevel;
static_assert(CONFIG_DRIVER_BUZZER_GPIO != 4, "GPIO4 is camera I2C SDA; move buzzer to GPIO14");
#endif
} // namespace

void DriverAlert::init()
{
#if CONFIG_DRIVER_BUZZER_ENABLED
    gpio_set_level(kBuzzerPin, kInactiveLevel);
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << CONFIG_DRIVER_BUZZER_GPIO;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
    // Short power-on chirp confirms the buzzer wiring and active level.
    gpio_set_level(kBuzzerPin, kActiveLevel);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(kBuzzerPin, kInactiveLevel);
    configASSERT(xTaskCreate(buzzer_task, "driver_buzzer", 2048, this, 4, nullptr) == pdPASS);
#endif
}

bool DriverAlert::update(DriverBehavior &behavior)
{
    const uint64_t now = esp_timer_get_time();
    const uint64_t window_us = static_cast<uint64_t>(CONFIG_DRIVER_ALERT_WINDOW_SECONDS) * 1000000;

    const uint32_t new_blinks = behavior.blink_count - m_last_blink_count;
    for (uint32_t i = 0; i < std::min<uint32_t>(new_blinks, m_blink_times.size()); ++i) {
        m_blink_times[m_blink_next++ % m_blink_times.size()] = now;
    }
    m_last_blink_count = behavior.blink_count;

    const uint32_t new_yawns = behavior.yawn_count - m_last_yawn_count;
    for (uint32_t i = 0; i < std::min<uint32_t>(new_yawns, m_yawn_times.size()); ++i) {
        m_yawn_times[m_yawn_next++ % m_yawn_times.size()] = now;
    }
    m_last_yawn_count = behavior.yawn_count;

    behavior.blink_window_count = count_recent(m_blink_times, now, window_us);
    behavior.yawn_window_count = count_recent(m_yawn_times, now, window_us);
    if (behavior.eyes_valid && behavior.eyes_closed &&
        behavior.eye_closed_duration_s * 1000.0f >= CONFIG_DRIVER_ALERT_EYES_CLOSED_MS) {
        behavior.alert_reasons |= DRIVER_ALERT_EYES_CLOSED;
    }
    // A completed yawn alert starts a fresh count of three new yawns.
    if (m_yawn_armed && now - m_yawn_start_us.load() >= kYawnBurstDurationUs) {
        m_yawn_armed = false;
        m_yawn_start_us.store(0);
        m_yawn_times.fill(0);
        m_yawn_next = 0;
        behavior.yawn_window_count = 0;
    } else if (behavior.driver_present &&
               behavior.yawn_window_count >= CONFIG_DRIVER_ALERT_YAWN_LIMIT && !m_yawn_armed) {
        m_yawn_start_us.store(now);
        m_yawn_armed = true;
    }
    if (behavior.driver_present && behavior.yawn_window_count >= CONFIG_DRIVER_ALERT_YAWN_LIMIT) {
        behavior.alert_reasons |= DRIVER_ALERT_YAWN_RATE;
    }
    const bool perclos_triggered = behavior.perclos_valid && behavior.perclos_window_ready &&
        behavior.perclos * 100.0f >= CONFIG_DRIVER_ALERT_PERCLOS_PERCENT;
    bool reset_perclos = false;
    if (m_perclos_armed && now - m_perclos_start_us.load() >= kPerclosBurstDurationUs) {
        m_perclos_armed = false;
        m_perclos_start_us.store(0);
        behavior.perclos_valid = false;
        behavior.perclos_window_ready = false;
        behavior.perclos = 0;
        behavior.perclos_observed_s = 0;
        reset_perclos = true;
    } else if (behavior.driver_present && perclos_triggered && !m_perclos_armed) {
        m_perclos_start_us.store(now);
        m_perclos_armed = true;
    }
    if (behavior.driver_present && perclos_triggered && !reset_perclos) {
        behavior.alert_reasons |= DRIVER_ALERT_PERCLOS;
    }
    if (behavior.driver_present) {
        m_no_face_since_us = 0;
        m_no_face_alert_start_us.store(0);
    } else if (!m_no_face_since_us) {
        m_no_face_since_us = now;
    } else if (now - m_no_face_since_us >=
               static_cast<uint64_t>(CONFIG_DRIVER_ALERT_NO_FACE_SECONDS) * 1000000) {
        if (!m_no_face_alert_start_us.load()) {
            m_no_face_alert_start_us.store(now);
        }
        behavior.alert_reasons |= DRIVER_ALERT_NO_FACE;
    }
    m_driver_present.store(behavior.driver_present);
#if CONFIG_DRIVER_BUZZER_ENABLED
    behavior.buzzer_pattern = active_pattern(now, behavior.alert_reasons, behavior.driver_present,
                                             m_yawn_start_us.load(), m_perclos_start_us.load());
#endif
    m_last_update_us.store(now);
    m_reasons.store(behavior.alert_reasons);
    return reset_perclos;
}

void DriverAlert::buzzer_task(void *arg)
{
#if CONFIG_DRIVER_BUZZER_ENABLED
    auto *self = static_cast<DriverAlert *>(arg);
    while (true) {
        const uint64_t now = esp_timer_get_time();
        const uint8_t reasons = self->m_reasons.load();
        const bool fresh = now - self->m_last_update_us.load() < 500000;
        const uint64_t yawn_start_us = self->m_yawn_start_us.load();
        const uint64_t perclos_start_us = self->m_perclos_start_us.load();
        const uint64_t no_face_start_us = self->m_no_face_alert_start_us.load();
        const uint8_t pattern = fresh ? active_pattern(now, reasons, self->m_driver_present.load(),
                                                       yawn_start_us, perclos_start_us)
                                      : static_cast<uint8_t>(DRIVER_BUZZER_OFF);
        bool sound = false;
        if (pattern == DRIVER_BUZZER_EYES) {
            sound = (now / 1000 % 500) < 200;
        } else if (pattern == DRIVER_BUZZER_PERCLOS_TRIPLE) {
            sound = burst_pulse(now, perclos_start_us, 3);
        } else if (pattern == DRIVER_BUZZER_YAWN_DOUBLE) {
            sound = burst_pulse(now, yawn_start_us, 2);
        } else if (pattern == DRIVER_BUZZER_NO_FACE_SLOW && no_face_start_us) {
            sound = ((now - no_face_start_us) / 1000 % kNoFacePeriodMs) < 160;
        }
        gpio_set_level(kBuzzerPin, sound ? kActiveLevel : kInactiveLevel);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#else
    (void)arg;
#endif
}
