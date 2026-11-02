#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver_behavior.hpp"

enum DriverAlertReason : uint8_t {
    DRIVER_ALERT_EYES_CLOSED = 1,
    DRIVER_ALERT_YAWN_RATE = 4,
    DRIVER_ALERT_PERCLOS = 8,
    DRIVER_ALERT_NO_FACE = 16,
};

enum DriverBuzzerPattern : uint8_t {
    DRIVER_BUZZER_OFF = 0,
    DRIVER_BUZZER_EYES = 1,
    DRIVER_BUZZER_YAWN_DOUBLE = 2,
    DRIVER_BUZZER_PERCLOS_TRIPLE = 3,
    DRIVER_BUZZER_NO_FACE_SLOW = 4,
};

class DriverAlert {
public:
    void init();
    // Returns true when the 15-second PERCLOS alarm has finished and its
    // observation window must be cleared before the next camera frame.
    bool update(DriverBehavior &behavior);

private:
    std::array<uint64_t, 120> m_blink_times{};
    std::array<uint64_t, 32> m_yawn_times{};
    size_t m_blink_next = 0;
    size_t m_yawn_next = 0;
    uint32_t m_last_blink_count = 0;
    uint32_t m_last_yawn_count = 0;
    bool m_yawn_armed = false;
    bool m_perclos_armed = false;
    uint64_t m_no_face_since_us = 0;
    std::atomic<uint8_t> m_reasons{0};
    std::atomic<bool> m_driver_present{false};
    std::atomic<uint64_t> m_yawn_start_us{0};
    std::atomic<uint64_t> m_perclos_start_us{0};
    std::atomic<uint64_t> m_no_face_alert_start_us{0};
    std::atomic<uint64_t> m_last_update_us{0};

    static void buzzer_task(void *arg);
};
