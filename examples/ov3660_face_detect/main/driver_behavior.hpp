#pragma once

#include <cstdint>
#include <array>
#include "who_detect.hpp"

// Experimental, camera-only observations. These are not validated safety
// decisions; thresholds must be calibrated for the installed camera and light.
struct DriverBehavior {
    bool driver_present = false;
    bool face_holdover = false;
    bool eyes_valid = false;
    bool eyes_closed = false;
    bool mouth_valid = false;
    bool mouth_open = false;
    bool perclos_valid = false;
    bool perclos_window_ready = false;
    bool face_luma_valid = false;
    uint8_t scene_luma = 0;
    uint8_t face_luma = 0;
    float left_eye_aperture = 0;
    float right_eye_aperture = 0;
    float left_eye_dark_fraction = 0;
    float right_eye_dark_fraction = 0;
    float left_eye_contrast = 0;
    float right_eye_contrast = 0;
    float mouth_aperture = 0;
    float mouth_contrast = 0;
    float eye_closed_probability = 0;
    float mouth_open_probability = 0;
    float eye_closed_duration_s = 0;
    float perclos = 0;
    float perclos_observed_s = 0;
    uint32_t blink_count = 0;
    uint32_t yawn_count = 0;
    uint32_t blink_window_count = 0;
    uint32_t yawn_window_count = 0;
    uint8_t alert_reasons = 0;
    uint8_t buzzer_pattern = 0;
    uint32_t uptime_ms = 0;
    std::array<int, 4> face_box{};
    std::array<int, 10> face_keypoints{};
};

class DriverBehaviorAnalyzer {
public:
    DriverBehavior update(const who::detect::WhoDetect::result_t &result);
    void reset_perclos();

private:
    struct PerclosBucket {
        uint32_t second = UINT32_MAX;
        uint32_t observed_us = 0;
        uint32_t closed_us = 0;
    };

    PerclosBucket m_perclos[60]{};
    uint64_t m_last_eye_us = 0;
    uint64_t m_first_eye_us = 0;
    uint64_t m_perclos_reset_us = 0;
    uint64_t m_last_face_us = 0;
    std::array<int, 4> m_last_face_box{};
    std::array<int, 10> m_last_face_keypoints{};
    uint64_t m_last_mouth_us = 0;
    uint64_t m_eye_closed_since_us = 0;
    uint64_t m_mouth_open_since_us = 0;
    uint64_t m_mouth_closed_since_us = 0;
    bool m_last_eyes_closed = false;
    uint8_t m_closed_candidate_frames = 0;
    uint8_t m_open_candidate_frames = 0;
    bool m_last_mouth_open = false;
    uint8_t m_mouth_open_candidate_frames = 0;
    uint8_t m_mouth_closed_candidate_frames = 0;
    bool m_yawn_counted = false;
    uint32_t m_blink_count = 0;
    uint32_t m_yawn_count = 0;
};
