#include "driver_lighting.hpp"

#include <algorithm>
#include "driver_behavior.hpp"
#include "video_capture.hpp"
#include "esp_video_ioctl.h"
#include "esp_timer.h"

namespace {
VideoCapture *camera = nullptr;
bool supported = false;
bool have_luma = false;
bool last_was_face = false;
bool bright_background = false;
float smoothed_luma = 0;
int level = -2;
int64_t last_adjust_us = 0;
int64_t last_face_us = 0;
int64_t settling_until_us = 0;
constexpr int kMinLevel = -4;
constexpr int kMaxLevel = 3;
constexpr int kBacklitMaxLevel = 1;
constexpr float kFaceLow = 82.0f;
constexpr float kFaceHigh = 112.0f;
constexpr int64_t kAdjustPeriodUs = 1500000;

bool set_level(int next)
{
    if (camera && camera->set_sensor_control(V4L2_CTRL_CLASS_CAMERA,
                                             V4L2_CID_CAMERA_AE_LEVEL, next) == ESP_OK) {
        level = next;
        return true;
    }
    return false;
}
}

void driver_lighting_init(VideoCapture *capture)
{
    camera = capture;
    have_luma = false;
    last_was_face = false;
    bright_background = false;
    last_face_us = 0;
    supported = set_level(-2);
    last_adjust_us = esp_timer_get_time();
    settling_until_us = last_adjust_us + kAdjustPeriodUs;
}

void driver_lighting_update(DriverBehavior &behavior)
{
    const int64_t now = esp_timer_get_time();
    const bool face = behavior.face_luma_valid && !behavior.face_holdover &&
                      behavior.face_box[2] - behavior.face_box[0] >= 68 &&
                      behavior.face_box[3] - behavior.face_box[1] >= 75;
    if (face) {
        last_face_us = now;
        // With a bright wall/lamp behind the driver, whole-sensor AE cannot
        // independently brighten the face. Chasing a high face target instead
        // pushes gain/exposure to the limit and shifts the trained eye patches.
        // Hysteresis keeps this ceiling from switching on every small movement.
        const int background_delta = static_cast<int>(behavior.scene_luma) - behavior.face_luma;
        if (behavior.scene_luma >= 160 && background_delta >= 55) {
            bright_background = true;
        } else if (behavior.scene_luma <= 140 || background_delta <= 35) {
            bright_background = false;
        }
    } else if (!last_face_us || now - last_face_us > 3000000) {
        bright_background = false;
    }
    // Short detector misses must not make the controller chase the background.
    const bool sample = face || !last_face_us || now - last_face_us > 3000000;
    if (sample) {
        const float luma = face ? behavior.face_luma : behavior.scene_luma;
        if (!have_luma || face != last_was_face) {
            smoothed_luma = luma;
            have_luma = true;
        } else {
            smoothed_luma += 0.15f * (luma - smoothed_luma);
        }
        last_was_face = face;
        // Preserve eye contrast instead of trying to make skin look bright in
        // the preview. Dim scenes can still use the original +3 upper limit.
        const float low = face ? kFaceLow : 80.0f;
        const float high = face ? kFaceHigh : 155.0f;
        const int ceiling = face && bright_background ? kBacklitMaxLevel : kMaxLevel;
        if (camera && now - last_adjust_us >= kAdjustPeriodUs) {
            int direction = smoothed_luma < low ? 1 : smoothed_luma > high ? -1 : 0;
            if (level > ceiling) {
                direction = -1;
            } else if (level == ceiling && direction > 0) {
                direction = 0;
            }
            if (!face) {
                // Do not chase a ceiling lamp so far down that a returning
                // face is too dark for the detector to find again.
                if (level < -2) direction = 1;
                else if (level > 1) direction = -1;
                else if ((level == -2 && direction < 0) || (level == 1 && direction > 0)) direction = 0;
            }
            const int next = std::clamp(level + direction, kMinLevel, kMaxLevel);
            if (!supported || next != level) {
                supported = set_level(next);
                settling_until_us = now + kAdjustPeriodUs;
            }
            last_adjust_us = now;
        }
    }
    behavior.auto_light = supported;
    behavior.ae_level = static_cast<int8_t>(level);
    if (!supported || !have_luma) {
        behavior.lighting_state = 0;
    } else if (smoothed_luma < 55.0f) {
        behavior.lighting_state = 3;
    } else if (smoothed_luma > 205.0f) {
        behavior.lighting_state = 4;
    } else {
        behavior.lighting_state = now < settling_until_us ? 1 : 2;
    }
}

const char *driver_lighting_state_name(unsigned state)
{
    switch (state) {
    case 1: return "ADJUST";
    case 2: return "OK";
    case 3: return "DARK";
    case 4: return "BRIGHT";
    default: return "UNAVAILABLE";
    }
}
