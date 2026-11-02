#include "driver_behavior.hpp"

#include <algorithm>
#include <cmath>

#include "esp_timer.h"
#include "driver_ml_weights.hpp"

namespace {
using dl::image::img_t;

int gray_at(const img_t &img, int x, int y)
{
    const auto *data = static_cast<const uint8_t *>(img.data);
    const size_t pos = (static_cast<size_t>(y) * img.width + x) * 2;
    const uint16_t pixel = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
    const int red = ((pixel >> 11) & 31) * 255 / 31;
    const int green = ((pixel >> 5) & 63) * 255 / 63;
    const int blue = (pixel & 31) * 255 / 31;
    return (77 * red + 150 * green + 29 * blue) >> 8;
}

float patch_mean(const img_t &img, int cx, int cy, int half_w, int half_h)
{
    const int x1 = std::max(0, cx - half_w);
    const int x2 = std::min(static_cast<int>(img.width) - 1, cx + half_w);
    const int y1 = std::max(0, cy - half_h);
    const int y2 = std::min(static_cast<int>(img.height) - 1, cy + half_h);
    if (x2 < x1 || y2 < y1) {
        return 0;
    }
    int sum = 0;
    for (int y = y1; y <= y2; ++y) {
        for (int x = x1; x <= x2; ++x) {
            sum += gray_at(img, x, y);
        }
    }
    return static_cast<float>(sum) / ((x2 - x1 + 1) * (y2 - y1 + 1));
}

int dark_vertical_run(const img_t &img, int cx, int cy, int half_w, int half_h, float skin, int row_percent)
{
    const int x1 = std::max(0, cx - half_w);
    const int x2 = std::min(static_cast<int>(img.width) - 1, cx + half_w);
    const int y1 = std::max(0, cy - half_h);
    const int y2 = std::min(static_cast<int>(img.height) - 1, cy + half_h);
    if (x2 < x1 || y2 < y1) {
        return 0;
    }
    const int darkness = static_cast<int>(skin * 0.60f);
    const int width = x2 - x1 + 1;
    int run = 0;
    int best = 0;
    for (int y = y1; y <= y2; ++y) {
        int dark = 0;
        for (int x = x1; x <= x2; ++x) {
            dark += gray_at(img, x, y) < darkness;
        }
        if (dark * 100 >= width * row_percent) {
            best = std::max(best, ++run);
        } else {
            run = 0;
        }
    }
    return best;
}

float dark_fraction(const img_t &img, int cx, int cy, int half_w, int half_h, float skin)
{
    const int x1 = std::max(0, cx - half_w);
    const int x2 = std::min(static_cast<int>(img.width) - 1, cx + half_w);
    const int y1 = std::max(0, cy - half_h);
    const int y2 = std::min(static_cast<int>(img.height) - 1, cy + half_h);
    if (x2 < x1 || y2 < y1) {
        return 0;
    }
    const int darkness = static_cast<int>(skin * 0.60f);
    int dark = 0;
    for (int y = y1; y <= y2; ++y) {
        for (int x = x1; x <= x2; ++x) {
            dark += gray_at(img, x, y) < darkness;
        }
    }
    return static_cast<float>(dark) / ((x2 - x1 + 1) * (y2 - y1 + 1));
}

float patch_contrast(const img_t &img, int cx, int cy, int half_w, int half_h, float skin)
{
    const int x1 = std::max(0, cx - half_w);
    const int x2 = std::min(static_cast<int>(img.width) - 1, cx + half_w);
    const int y1 = std::max(0, cy - half_h);
    const int y2 = std::min(static_cast<int>(img.height) - 1, cy + half_h);
    if (x2 < x1 || y2 < y1 || skin <= 0) {
        return 0;
    }
    uint32_t sum = 0;
    uint64_t sum_sq = 0;
    for (int y = y1; y <= y2; ++y) {
        for (int x = x1; x <= x2; ++x) {
            const int gray = gray_at(img, x, y);
            sum += gray;
            sum_sq += gray * gray;
        }
    }
    const int count = (x2 - x1 + 1) * (y2 - y1 + 1);
    const float mean = static_cast<float>(sum) / count;
    const float variance = static_cast<float>(sum_sq) / count - mean * mean;
    return std::sqrt(std::max(0.0f, variance)) / skin;
}

bool inside(const img_t &img, int x, int y)
{
    return x >= 0 && y >= 0 && x < img.width && y < img.height;
}

float gray_sample(const img_t &img, float x, float y)
{
    const int px = std::clamp(static_cast<int>(std::round(x)), 0, static_cast<int>(img.width) - 1);
    const int py = std::clamp(static_cast<int>(std::round(y)), 0, static_cast<int>(img.height) - 1);
    return static_cast<float>(gray_at(img, px, py));
}

float grid_mean(const img_t &img, float cx, float cy, float half_w, float half_h)
{
    float sum = 0;
    for (int iy = 0; iy < 3; ++iy) {
        for (int ix = 0; ix < 3; ++ix) {
            sum += gray_sample(img, cx + half_w * (ix - 1), cy + half_h * (iy - 1));
        }
    }
    return sum / 9.0f;
}

uint8_t region_luma(const img_t &img, int x1, int y1, int x2, int y2)
{
    x1 = std::clamp(x1, 0, static_cast<int>(img.width) - 1);
    y1 = std::clamp(y1, 0, static_cast<int>(img.height) - 1);
    x2 = std::clamp(x2, x1 + 1, static_cast<int>(img.width));
    y2 = std::clamp(y2, y1 + 1, static_cast<int>(img.height));
    int sum = 0;
    for (int iy = 0; iy < 8; ++iy) {
        const int y = y1 + (y2 - y1 - 1) * (2 * iy + 1) / 16;
        for (int ix = 0; ix < 8; ++ix) {
            const int x = x1 + (x2 - x1 - 1) * (2 * ix + 1) / 16;
            sum += gray_at(img, x, y);
        }
    }
    return static_cast<uint8_t>((sum + 32) / 64);
}

float logistic(float x)
{
    return 1.0f / (1.0f + std::exp(-x));
}

// Small offline-trained MLP. All pixel sampling and inference run on ESP32-S3.
float eye_closed_probability(const img_t &img, int x, int y, int face_w, int face_h)
{
    using namespace driver_ml_weights;
    const float skin = grid_mean(img, x, y + face_h * 0.10f,
                                 face_w * 0.05f, face_h * 0.02f);
    if (skin < 25.0f) {
        return -1.0f;
    }
    float hidden[24];
    std::copy_n(eye_b1, 24, hidden);
    for (int iy = 0; iy < 12; ++iy) {
        for (int ix = 0; ix < 12; ++ix) {
            const int i = iy * 12 + ix;
            const float px = x + face_w * 0.105f * (2.0f * ix / 11.0f - 1.0f);
            const float py = y + face_h * 0.070f * (2.0f * iy / 11.0f - 1.0f);
            const float value = std::clamp(gray_sample(img, px, py) / skin, 0.0f, 2.0f);
            const float scaled = (value - eye_mean[i]) * eye_inv_scale[i];
            for (int h = 0; h < 24; ++h) {
                hidden[h] += scaled * eye_w1[i * 24 + h];
            }
        }
    }
    float logit = eye_b2;
    for (int h = 0; h < 24; ++h) {
        logit += std::max(0.0f, hidden[h]) * eye_w2[h];
    }
    return logistic(logit);
}

float mouth_open_probability(const img_t &img, const std::array<int, 10> &p,
                             int face_w, int face_h)
{
    using namespace driver_ml_weights;
    const float mx = (p[2] + p[8]) * 0.5f;
    const float my = (p[3] + p[9]) * 0.5f;
    const float cheek_w = face_w * 0.03f;
    const float cheek_h = face_h * 0.03f;
    const float skin = (grid_mean(img, mx - face_w * 0.33f, my, cheek_w, cheek_h) +
                        grid_mean(img, mx + face_w * 0.33f, my, cheek_w, cheek_h)) * 0.5f;
    if (skin < 25.0f) {
        return -1.0f;
    }
    float logit = mouth_b;
    for (int iy = 0; iy < 12; ++iy) {
        for (int ix = 0; ix < 12; ++ix) {
            const int i = iy * 12 + ix;
            const float px = mx + face_w * 0.18f * (2.0f * ix / 11.0f - 1.0f);
            const float py = my + face_h * 0.15f * (2.0f * iy / 11.0f - 1.0f);
            const float value = std::clamp(gray_sample(img, px, py) / skin, 0.0f, 2.0f);
            logit += (value - mouth_mean[i]) * mouth_inv_scale[i] * mouth_w[i];
        }
    }
    return logistic(logit);
}
} // namespace

DriverBehavior DriverBehaviorAnalyzer::update(const who::detect::WhoDetect::result_t &result)
{
    DriverBehavior out;
    const uint64_t now = esp_timer_get_time();
    out.uptime_ms = static_cast<uint32_t>(now / 1000);
    out.blink_count = m_blink_count;
    out.yawn_count = m_yawn_count;
    const auto &img = result.img;

    if (img.data && img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB565BE) {
        out.scene_luma = region_luma(img, 0, 0, img.width, img.height);
        bool have_landmarks = false;
        if (!result.det_res.empty()) {
            const auto face = std::max_element(result.det_res.begin(), result.det_res.end(),
                                               [](const auto &a, const auto &b) { return a.score < b.score; });
            out.driver_present = true;
            std::copy_n(face->box.begin(), 4, out.face_box.begin());
            if (out.face_box[2] > out.face_box[0] && out.face_box[3] > out.face_box[1]) {
                out.face_luma = region_luma(img, out.face_box[0], out.face_box[1],
                                            out.face_box[2], out.face_box[3]);
                out.face_luma_valid = true;
            }
            if (face->keypoint.size() == 10) {
                std::copy_n(face->keypoint.begin(), 10, out.face_keypoints.begin());
                const int width = out.face_box[2] - out.face_box[0];
                const int height = out.face_box[3] - out.face_box[1];
                // Training examples include faces down to 68x75 pixels.
                if (width >= 68 && height >= 75) {
                    have_landmarks = true;
                    m_last_face_us = now;
                    m_last_face_box = out.face_box;
                    m_last_face_keypoints = out.face_keypoints;
                }
            }
        } else if (m_last_face_us && now - m_last_face_us <= 1200000) {
            // Keep the last landmarks briefly when a wide yawn makes the
            // detector miss a face. This assumes the head moves little.
            out.driver_present = true;
            out.face_holdover = true;
            out.face_box = m_last_face_box;
            out.face_keypoints = m_last_face_keypoints;
            have_landmarks = true;
        }
        if (have_landmarks) {
            const int face_w = out.face_box[2] - out.face_box[0];
            const int face_h = out.face_box[3] - out.face_box[1];
            const auto &p = out.face_keypoints;
            const int le_x = p[0], le_y = p[1];
            const int re_x = p[6], re_y = p[7];
            // A turned head can hide one eye while the face detector still
            // reports both landmarks. Nose offset relative to eye spacing is
            // a cheap pose check that works with the existing five points.
            const int eye_spacing = re_x - le_x;
            const float eye_pose_deviation = eye_spacing > 0
                                                 ? std::abs(p[4] - (le_x + re_x) * 0.5f) / eye_spacing
                                                 : 1.0f;
            const bool eye_pose_reliable = eye_pose_deviation <= 0.18f;
            const int ml_x = p[2], ml_y = p[3];
            const int mr_x = p[8], mr_y = p[9];
            if (inside(img, le_x, le_y) && inside(img, re_x, re_y) &&
                inside(img, ml_x, ml_y) && inside(img, mr_x, mr_y)) {
                const int eye_skin_y = std::max(6, static_cast<int>(face_h * 0.10f));
                const int eye_skin_w = std::max(4, static_cast<int>(face_w * 0.06f));
                const int eye_skin_h = std::max(2, static_cast<int>(face_h * 0.02f));
                const float left_skin = patch_mean(img, le_x, le_y + eye_skin_y, eye_skin_w, eye_skin_h);
                const float right_skin = patch_mean(img, re_x, re_y + eye_skin_y, eye_skin_w, eye_skin_h);
                // Reused landmarks can drift onto the background when the
                // head moves; do not infer eyes from a held face box.
                if (!out.face_holdover && eye_pose_reliable &&
                    left_skin >= 25 && right_skin >= 25) {
                    const int eye_w = std::max(5, static_cast<int>(face_w * 0.06f));
                    const int eye_h = std::max(4, static_cast<int>(face_h * 0.045f));
                    out.left_eye_aperture =
                        static_cast<float>(dark_vertical_run(img, le_x, le_y, eye_w, eye_h, left_skin, 25)) / face_h;
                    out.right_eye_aperture =
                        static_cast<float>(dark_vertical_run(img, re_x, re_y, eye_w, eye_h, right_skin, 25)) / face_h;
                    out.left_eye_dark_fraction =
                        dark_fraction(img, le_x, le_y, eye_w, eye_h, left_skin);
                    out.right_eye_dark_fraction =
                        dark_fraction(img, re_x, re_y, eye_w, eye_h, right_skin);
                    out.left_eye_contrast =
                        patch_contrast(img, le_x, le_y, eye_w, eye_h, left_skin);
                    out.right_eye_contrast =
                        patch_contrast(img, re_x, re_y, eye_w, eye_h, right_skin);
                    const float left_probability =
                        eye_closed_probability(img, le_x, le_y, face_w, face_h);
                    const float right_probability =
                        eye_closed_probability(img, re_x, re_y, face_w, face_h);
                    if (left_probability >= 0 && right_probability >= 0) {
                        out.eyes_valid = true;
                        out.eye_closed_probability = (left_probability + right_probability) * 0.5f;
                        // Near frontal, preserve short blinks. At a mild
                        // angle, require two strong frames so a partially
                        // occluded eye cannot create a false closure.
                        if (eye_pose_deviation <= 0.10f) {
                            m_closed_candidate_frames = out.eye_closed_probability >= 0.80f
                                                            ? 2
                                                            : out.eye_closed_probability >= 0.50f
                                                                  ? std::min<int>(m_closed_candidate_frames + 1, 2)
                                                                  : 0;
                        } else {
                            m_closed_candidate_frames = out.eye_closed_probability >= 0.65f
                                                            ? std::min<int>(m_closed_candidate_frames + 1, 2)
                                                            : 0;
                        }
                        m_open_candidate_frames = out.eye_closed_probability < 0.50f
                                                      ? std::min<int>(m_open_candidate_frames + 1, 2)
                                                      : 0;
                        out.eyes_closed = m_last_eyes_closed;
                        if (m_closed_candidate_frames >= 2) {
                            out.eyes_closed = true;
                        } else if (m_open_candidate_frames >= 2) {
                            out.eyes_closed = false;
                        }
                    }
                }

                const int mouth_x = (ml_x + mr_x) / 2;
                const int mouth_y = (ml_y + mr_y) / 2;
                const int cheek_offset = std::max(8, static_cast<int>(face_w * 0.33f));
                const int cheek_w = std::max(2, static_cast<int>(face_w * 0.03f));
                const int cheek_h = std::max(2, static_cast<int>(face_h * 0.03f));
                const float cheek_left =
                    patch_mean(img, mouth_x - cheek_offset, mouth_y, cheek_w, cheek_h);
                const float cheek_right =
                    patch_mean(img, mouth_x + cheek_offset, mouth_y, cheek_w, cheek_h);
                const float mouth_skin = (cheek_left + cheek_right) * 0.5f;
                if (mouth_skin >= 25) {
                    const int mouth_w = std::max(5, static_cast<int>(face_w * 0.065f));
                    const int mouth_h = std::max(8, static_cast<int>(face_h * 0.12f));
                    out.mouth_aperture = static_cast<float>(
                        dark_vertical_run(img, mouth_x, mouth_y, mouth_w, mouth_h, mouth_skin, 35)) / face_h;
                    out.mouth_contrast =
                        patch_contrast(img, mouth_x, mouth_y, mouth_w, mouth_h, mouth_skin);
                    const float probability = mouth_open_probability(img, p, face_w, face_h);
                    if (probability >= 0) {
                        out.mouth_valid = true;
                        out.mouth_open_probability = probability;
                        m_mouth_open_candidate_frames = probability >= 0.5f
                                                            ? std::min<int>(m_mouth_open_candidate_frames + 1, 2)
                                                            : 0;
                        m_mouth_closed_candidate_frames = probability < 0.5f
                                                              ? std::min<int>(m_mouth_closed_candidate_frames + 1, 2)
                                                              : 0;
                        out.mouth_open = m_last_mouth_open;
                        if (m_mouth_open_candidate_frames >= 2) {
                            out.mouth_open = true;
                        } else if (m_mouth_closed_candidate_frames >= 2) {
                            out.mouth_open = false;
                        }
                    }
                }
            }
        }
    }

    if (out.eyes_valid) {
        if (!m_first_eye_us) {
            m_first_eye_us = now;
        }
        if (m_last_eye_us && m_last_eye_us > m_perclos_reset_us &&
            now > m_last_eye_us && now - m_last_eye_us <= 500000) {
            const uint32_t duration_us = std::min<uint64_t>(now - m_last_eye_us, 250000);
            const uint32_t sec = static_cast<uint32_t>(now / 1000000);
            auto &bucket = m_perclos[sec % 60];
            if (bucket.second != sec) {
                bucket = {sec, 0, 0};
            }
            bucket.observed_us += duration_us;
            if (m_last_eyes_closed) {
                bucket.closed_us += duration_us;
            }
        }
        if (out.eyes_closed && !m_last_eyes_closed) {
            m_eye_closed_since_us = now;
        } else if (!out.eyes_closed && m_last_eyes_closed && m_eye_closed_since_us) {
            const uint64_t closed_us = now - m_eye_closed_since_us;
            if (closed_us >= 80000 && closed_us <= 1200000) {
                ++m_blink_count;
            }
            m_eye_closed_since_us = 0;
        }
        if (out.eyes_closed && m_eye_closed_since_us) {
            out.eye_closed_duration_s = static_cast<float>(now - m_eye_closed_since_us) / 1000000.0f;
        }
        m_last_eyes_closed = out.eyes_closed;
        m_last_eye_us = now;
    } else {
        // A profile view is an explicit loss of eye visibility: discard the
        // old closure immediately so returning to front cannot count a blink.
        const auto &p = out.face_keypoints;
        const int eye_spacing = p[6] - p[0];
        const bool profile_view = out.driver_present && !out.face_holdover &&
                                  eye_spacing > 0 &&
                                  std::abs(p[4] - (p[0] + p[6]) * 0.5f) >
                                      eye_spacing * 0.18f;
        if (profile_view || !m_last_eye_us || now - m_last_eye_us > 500000) {
            m_last_eye_us = 0;
            m_last_eyes_closed = false;
            m_eye_closed_since_us = 0;
            m_closed_candidate_frames = 0;
            m_open_candidate_frames = 0;
        }
    }

    if (out.mouth_valid) {
        m_last_mouth_us = now;
        if (out.mouth_open && !m_last_mouth_open) {
            m_mouth_open_since_us = now;
        }
        if (out.mouth_open) {
            m_mouth_closed_since_us = 0;
        }
        if (out.mouth_open && m_mouth_open_since_us && !m_yawn_counted &&
            now - m_mouth_open_since_us >= 800000) {
            ++m_yawn_count;
            m_yawn_counted = true;
        }
        if (!out.mouth_open) {
            m_mouth_open_since_us = 0;
            if (!m_mouth_closed_since_us) {
                m_mouth_closed_since_us = now;
            }
            if (now - m_mouth_closed_since_us >= 1000000) {
                m_yawn_counted = false;
            }
        }
        m_last_mouth_open = out.mouth_open;
    } else {
        if (!m_last_mouth_us || now - m_last_mouth_us > 500000) {
            m_last_mouth_us = 0;
            m_mouth_open_since_us = 0;
            m_last_mouth_open = false;
            m_mouth_closed_since_us = 0;
            m_mouth_open_candidate_frames = 0;
            m_mouth_closed_candidate_frames = 0;
        }
    }

    uint64_t observed = 0;
    uint64_t closed = 0;
    const uint32_t sec = static_cast<uint32_t>(now / 1000000);
    for (const auto &bucket : m_perclos) {
        if (bucket.second != UINT32_MAX && sec >= bucket.second && sec - bucket.second < 60) {
            observed += bucket.observed_us;
            closed += bucket.closed_us;
        }
    }
    out.perclos_observed_s = static_cast<float>(observed) / 1000000.0f;
    if (observed >= 2000000) {
        out.perclos_valid = true;
        out.perclos = static_cast<float>(closed) / observed;
        out.perclos_window_ready = m_first_eye_us && now - m_first_eye_us >= 60000000 &&
                                   observed >= 30000000;
    }
    out.blink_count = m_blink_count;
    out.yawn_count = m_yawn_count;
    return out;
}

void DriverBehaviorAnalyzer::reset_perclos()
{
    for (auto &bucket : m_perclos) {
        bucket = PerclosBucket{};
    }
    m_first_eye_us = 0;
    m_perclos_reset_us = esp_timer_get_time();
}
