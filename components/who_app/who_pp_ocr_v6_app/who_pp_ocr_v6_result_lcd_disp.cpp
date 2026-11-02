#include "who_pp_ocr_v6_result_lcd_disp.hpp"
#if !BSP_CONFIG_NO_GRAPHIC_LIB
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace who {
namespace lcd_disp {

static const char *TAG = "PPOCRV6Overlay";

// Material Design "300" tier — mid-luminance so labels stay legible on the
// black text panel.
std::vector<lv_color_t> WhoPPOCRV6ResultLCDDisp::default_palette()
{
    return {
        lv_color_hex(0xE57373),
        lv_color_hex(0xFF8A65),
        lv_color_hex(0xFFD54F),
        lv_color_hex(0xFFF176),
        lv_color_hex(0xAED581),
        lv_color_hex(0x81C784),
        lv_color_hex(0x4DB6AC),
        lv_color_hex(0x4FC3F7),
        lv_color_hex(0x7986CB),
        lv_color_hex(0xBA68C8),
        lv_color_hex(0xF06292),
        lv_color_hex(0xB39DDB),
    };
}

WhoPPOCRV6ResultLCDDisp::WhoPPOCRV6ResultLCDDisp(task::WhoTask *task,
                                                 lv_obj_t *canvas,
                                                 uint16_t ocr_input_w,
                                                 uint16_t ocr_input_h,
                                                 FontPicker font_picker,
                                                 const lv_font_t *default_font) :
    m_task(task),
    m_canvas(canvas),
    m_ocr_w(ocr_input_w),
    m_ocr_h(ocr_input_h),
    m_font_picker(font_picker),
    m_default_font(default_font),
    m_res_mutex(xSemaphoreCreateMutex()),
    m_palette(default_palette()),
    m_stable_buf(nullptr),
    m_stable_w(0),
    m_stable_h(0),
    m_text_panel(nullptr)
{
    // Backing-size fallback ladder — largest RGB565 buffer that still leaves
    // `k_min_headroom_bytes` largest-free PSRAM for the OCR runtime.
    struct Candidate {
        uint16_t w;
        uint16_t h;
    };

    lv_obj_t *parent_probe = lv_obj_get_parent(m_canvas);
    const int32_t screen_w =
        parent_probe ? static_cast<int32_t>(lv_obj_get_width(parent_probe)) : static_cast<int32_t>(ocr_input_w);
    const int32_t screen_h =
        parent_probe ? static_cast<int32_t>(lv_obj_get_height(parent_probe)) : static_cast<int32_t>(ocr_input_h);

    // Left canvas ≈ 40 % of horizontal budget, right text panel ≈ 60 %.
    constexpr int32_t k_reserved_bottom = 120;
    constexpr int32_t k_right_panel_reserve = 16 + 8 + 8;
    constexpr int32_t k_canvas_w_num = 2;
    constexpr int32_t k_canvas_w_den = 5;
    const int32_t canvas_h_budget = std::max<int32_t>(120, screen_h - k_reserved_bottom);
    const int32_t canvas_w_budget =
        std::max<int32_t>(120, (screen_w - k_right_panel_reserve) * k_canvas_w_num / k_canvas_w_den);

    const double ar = static_cast<double>(ocr_input_w) / static_cast<double>(ocr_input_h);
    int32_t fit_w = canvas_w_budget;
    int32_t fit_h = static_cast<int32_t>(std::lround(fit_w / ar));
    if (fit_h > canvas_h_budget) {
        fit_h = canvas_h_budget;
        fit_w = static_cast<int32_t>(std::lround(fit_h * ar));
    }
    fit_w = std::clamp(fit_w, static_cast<int32_t>(16), static_cast<int32_t>(ocr_input_w));
    fit_h = std::clamp(fit_h, static_cast<int32_t>(16), static_cast<int32_t>(ocr_input_h));

    ESP_LOGI(TAG,
             "canvas fit: screen %dx%d, ocr %ux%u, budget %dx%d → backing target %dx%d",
             (int)screen_w,
             (int)screen_h,
             (unsigned)ocr_input_w,
             (unsigned)ocr_input_h,
             (int)canvas_w_budget,
             (int)canvas_h_budget,
             (int)fit_w,
             (int)fit_h);

    auto make_cand = [&](int num, int den) -> Candidate {
        int32_t w = std::max<int32_t>(16, fit_w * num / den);
        int32_t h = std::max<int32_t>(16, fit_h * num / den);
        return {static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
    };
    const Candidate ladder[] = {
        make_cand(1, 1),
        make_cand(3, 4),
        make_cand(1, 2),
        make_cand(3, 8),
        make_cand(1, 4),
    };

    const size_t psram_free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG,
             "PSRAM before backing alloc: free=%u largest=%u (headroom target=%u)",
             (unsigned)psram_free_before,
             (unsigned)psram_largest_before,
             (unsigned)k_min_headroom_bytes);

    for (const auto &c : ladder) {
        const size_t sz = static_cast<size_t>(c.w) * c.h * 2;
        if (psram_largest_before < sz + k_min_headroom_bytes) {
            ESP_LOGW(TAG,
                     "skip %ux%u backing (%u B): would leave largest ~%u, need >=%u",
                     (unsigned)c.w,
                     (unsigned)c.h,
                     (unsigned)sz,
                     (unsigned)(psram_largest_before > sz ? psram_largest_before - sz : 0),
                     (unsigned)k_min_headroom_bytes);
            continue;
        }
        void *p = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            ESP_LOGW(
                TAG, "aligned_alloc %ux%u (%u B) failed despite headroom", (unsigned)c.w, (unsigned)c.h, (unsigned)sz);
            continue;
        }
        std::memset(p, 0, sz);
        m_stable_buf = p;
        m_stable_w = c.w;
        m_stable_h = c.h;
        break;
    }

    if (!m_stable_buf) {
        ESP_LOGE(TAG, "no PSRAM budget for any backing size — overlay display disabled");
        return;
    }

    m_stable_img = {.data = m_stable_buf,
                    .width = m_stable_w,
                    .height = m_stable_h,
                    .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE};

    ESP_LOGI(TAG,
             "backing: %ux%u RGB565 (%u B) @ %p, PSRAM after alloc: free=%u largest=%u",
             (unsigned)m_stable_w,
             (unsigned)m_stable_h,
             (unsigned)(static_cast<size_t>(m_stable_w) * m_stable_h * 2),
             m_stable_buf,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    bsp_display_lock(0);
    lv_canvas_set_buffer(m_canvas, m_stable_buf, m_stable_w, m_stable_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(m_canvas, m_stable_w, m_stable_h);

    lv_obj_t *parent = lv_obj_get_parent(m_canvas);
    const lv_coord_t parent_w = lv_obj_get_width(parent);
    const lv_coord_t canvas_left_pad = 8;
    const lv_coord_t canvas_top_pad = 8;
    const lv_coord_t gap = 16;
    const lv_coord_t right_pad = 8;
    lv_coord_t panel_w = parent_w - m_stable_w - canvas_left_pad - gap - right_pad;
    if (panel_w < m_stable_w) {
        panel_w = m_stable_w;
    }

    lv_obj_align(m_canvas, LV_ALIGN_TOP_LEFT, canvas_left_pad, canvas_top_pad);
    lv_obj_remove_flag(m_canvas, LV_OBJ_FLAG_HIDDEN);

    m_text_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(m_text_panel);
    lv_obj_set_size(m_text_panel, panel_w, m_stable_h);
    lv_obj_align_to(m_text_panel, m_canvas, LV_ALIGN_OUT_RIGHT_TOP, gap, 0);
    lv_obj_set_style_bg_color(m_text_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(m_text_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(m_text_panel, 0, 0);
    lv_obj_clear_flag(m_text_panel, LV_OBJ_FLAG_SCROLLABLE);
    // Let long labels bleed a few px past the panel's right edge into the
    // empty screen strip instead of getting truncated by LVGL clipping.
    lv_obj_add_flag(m_text_panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(m_text_panel, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

WhoPPOCRV6ResultLCDDisp::~WhoPPOCRV6ResultLCDDisp()
{
    if (m_text_panel) {
        bsp_display_lock(0);
        lv_obj_delete(m_text_panel);
        bsp_display_unlock();
        m_text_panel = nullptr;
    }
    if (m_stable_buf) {
        heap_caps_free(m_stable_buf);
    }
    vSemaphoreDelete(m_res_mutex);
}

void WhoPPOCRV6ResultLCDDisp::save_result(const ocr::WhoPPOCRV6::result_t &result,
                                          const dl::image::img_t &ocr_input_rgb888)
{
    if (!m_task->is_active() || !m_canvas || !m_stable_buf) {
        return;
    }

    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    m_result = result;
    xSemaphoreGive(m_res_mutex);

    m_transformer.set_src_img(ocr_input_rgb888).set_dst_img(m_stable_img).transform();

    bsp_display_lock(0);
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    draw_overlay_locked();

    if (m_text_panel) {
        lv_obj_remove_flag(m_text_panel, LV_OBJ_FLAG_HIDDEN);
    }

    // Repopulate the right panel: one label per line, each continuously
    // scaled with `transform_scale_*` to match its quad's on-screen size.
    // Row-major push-down avoidance nudges collisions downward; anything
    // that would land past the bottom is pinned instead of dropped.
    if (m_text_panel) {
        lv_obj_clean(m_text_panel);

        struct Pending {
            int32_t rel_x;
            int32_t rel_y;
            int32_t est_w;     // scaled visible width
            int32_t est_h;     // scaled visible height
            int32_t scale_256; // LVGL fixed-point (256 = 1.0×)
            const lv_font_t *font;
            lv_color_t color;
            size_t result_idx;
        };
        std::vector<Pending> pending;
        pending.reserve(m_result.size());

        const float sx = static_cast<float>(m_stable_w) / static_cast<float>(m_ocr_w);
        const float sy = static_cast<float>(m_stable_h) / static_cast<float>(m_ocr_h);
        const int32_t panel_w = lv_obj_get_width(m_text_panel);
        const int32_t panel_h = lv_obj_get_height(m_text_panel);

        int too_wide = 0;
        int shrunk_for_width = 0;
        int32_t min_scale_256 = std::numeric_limits<int32_t>::max();
        int32_t max_scale_256 = 0;

        constexpr int32_t k_panel_right_bleed = 8;

        // Visual-height correction. `line_height` includes ascender +
        // descender + line-gap; PP-OCR quads are the unclip-expanded hull of
        // the tight character body. num/den < 1 shrinks the label so its
        // visible strokes match the actual text pixels on the canvas (not
        // the quad's outer box). Bisected empirically: 1/1 too big, 3/4 too
        // small, 7/8 sits right for Noto Sans CJK 24 pt / 2 bpp.
        constexpr int32_t k_visual_scale_num = 7;
        constexpr int32_t k_visual_scale_den = 8;

        for (size_t i = 0; i < m_result.size(); ++i) {
            const auto &r = m_result[i];
            if (r.text.empty()) {
                continue;
            }

            int min_x = static_cast<int>(m_stable_w);
            int max_x = 0;
            int min_y_ocr = std::numeric_limits<int>::max();
            int max_y_ocr = std::numeric_limits<int>::min();
            for (int k = 0; k < 4; ++k) {
                int x = static_cast<int>(std::lround(r.box.points[2 * k] * sx));
                int y_ocr = static_cast<int>(std::lround(r.box.points[2 * k + 1]));
                if (x < min_x)
                    min_x = x;
                if (x > max_x)
                    max_x = x;
                if (y_ocr < min_y_ocr)
                    min_y_ocr = y_ocr;
                if (y_ocr > max_y_ocr)
                    max_y_ocr = y_ocr;
            }
            min_x = std::clamp(min_x, 0, static_cast<int>(m_stable_w) - 1);
            max_x = std::clamp(max_x, min_x + 1, static_cast<int>(m_stable_w));
            const int quad_w_display = std::max(1, max_x - min_x);
            const int quad_h_ocr = std::max(1, max_y_ocr - min_y_ocr);
            const int min_y_display =
                std::clamp(static_cast<int>(std::lround(min_y_ocr * sy)), 0, static_cast<int>(m_stable_h) - 1);
            const int quad_h_display = std::max(1, static_cast<int>(std::lround(quad_h_ocr * sy)));

            const lv_font_t *font = m_font_picker ? m_font_picker(quad_h_ocr) : m_default_font;
            if (!font)
                font = m_default_font;

            lv_point_t sz;
            lv_text_get_size(&sz, r.text.c_str(), font, 0, 0, 0, LV_TEXT_FLAG_EXPAND);
            const int32_t nat_w = sz.x > 0 ? sz.x : font->line_height;
            const int32_t nat_h = font->line_height;

            if (min_x >= panel_w) {
                ++too_wide;
                continue;
            }

            // Dual-axis fit: `min(scale_h, scale_w)` keeps the label inside
            // both quad axes and prevents "same quad height, different label
            // width" surprises when neighbours have very different text
            // lengths.
            const int32_t scale_h_256 = static_cast<int32_t>(
                std::lround((static_cast<float>(quad_h_display) / static_cast<float>(nat_h)) * 256.0f *
                            static_cast<float>(k_visual_scale_num) / static_cast<float>(k_visual_scale_den)));
            const int32_t scale_w_256 = static_cast<int32_t>(
                std::lround((static_cast<float>(quad_w_display) / static_cast<float>(nat_w)) * 256.0f *
                            static_cast<float>(k_visual_scale_num) / static_cast<float>(k_visual_scale_den)));
            int32_t scale_256 = std::min(scale_h_256, scale_w_256);
            scale_256 = std::clamp<int32_t>(scale_256, 128, 1024);

            // Panel-edge guard: `transform_scale` doesn't override the
            // screen's clip rect, so shrink if the scaled tail would fall
            // off the right edge of the LCD.
            const int32_t avail_w = std::max<int32_t>(1, panel_w + k_panel_right_bleed - min_x);
            const int32_t proposed_w = static_cast<int32_t>(std::lround(nat_w * (scale_256 / 256.0f)));
            if (proposed_w > avail_w && nat_w > 0) {
                scale_256 = static_cast<int32_t>(
                    std::lround((static_cast<float>(avail_w) / static_cast<float>(nat_w)) * 256.0f));
                scale_256 = std::clamp<int32_t>(scale_256, 128, 1024);
                ++shrunk_for_width;
            }

            const int32_t est_w = static_cast<int32_t>(std::lround(nat_w * (scale_256 / 256.0f)));
            const int32_t est_h = static_cast<int32_t>(std::lround(nat_h * (scale_256 / 256.0f)));

            if (scale_256 < min_scale_256)
                min_scale_256 = scale_256;
            if (scale_256 > max_scale_256)
                max_scale_256 = scale_256;

            Pending p;
            p.rel_x = min_x;
            p.rel_y = min_y_display;
            p.est_w = est_w;
            p.est_h = est_h;
            p.scale_256 = scale_256;
            p.font = font;
            p.color = m_palette[i % m_palette.size()];
            p.result_idx = i;
            pending.push_back(p);
        }

        // (rel_y, rel_x) sort ensures push-down only nudges labels further
        // down, never into a slot a later label needs.
        std::sort(pending.begin(), pending.end(), [](const Pending &a, const Pending &b) {
            if (a.rel_y != b.rel_y)
                return a.rel_y < b.rel_y;
            return a.rel_x < b.rel_x;
        });

        std::vector<Pending> placed;
        placed.reserve(pending.size());
        int32_t pinned = 0;
        constexpr int32_t k_min_gap = 2;

        for (auto cur : pending) {
            for (size_t guard = 0; guard <= placed.size(); ++guard) {
                bool collided = false;
                for (const auto &prev : placed) {
                    const int32_t px0 = prev.rel_x;
                    const int32_t px1 = prev.rel_x + prev.est_w;
                    const int32_t py0 = prev.rel_y;
                    const int32_t py1 = prev.rel_y + prev.est_h;
                    const int32_t cx0 = cur.rel_x;
                    const int32_t cx1 = cur.rel_x + cur.est_w;
                    const int32_t cy0 = cur.rel_y;
                    const int32_t cy1 = cur.rel_y + cur.est_h;
                    if (cx0 < px1 && cx1 > px0 && cy0 < py1 && cy1 > py0) {
                        cur.rel_y = py1 + k_min_gap;
                        collided = true;
                        break;
                    }
                }
                if (!collided)
                    break;
            }
            // Pin to bottom instead of dropping — a small overlap reads
            // better than a missing line.
            if (cur.rel_y + cur.est_h > panel_h) {
                cur.rel_y = std::max<int32_t>(0, panel_h - cur.est_h);
                ++pinned;
            }
            placed.push_back(cur);
        }

        // Pivot at (0, 0) so the scaled label's top-left equals rel_pos.
        // Requires CONFIG_LV_USE_CLIB_MALLOC=y — per-label layer buffers
        // fragment LVGL's built-in TLSF pool otherwise.
        for (const auto &p : placed) {
            lv_obj_t *lbl = lv_label_create(m_text_panel);
            lv_obj_remove_style_all(lbl);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lbl, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(lbl, p.font, 0);
            lv_obj_set_style_text_color(lbl, p.color, 0);
            lv_label_set_text(lbl, m_result[p.result_idx].text.c_str());
            lv_obj_set_pos(lbl, p.rel_x, p.rel_y);
            lv_obj_set_style_transform_pivot_x(lbl, 0, 0);
            lv_obj_set_style_transform_pivot_y(lbl, 0, 0);
            lv_obj_set_style_transform_scale_x(lbl, p.scale_256, 0);
            lv_obj_set_style_transform_scale_y(lbl, p.scale_256, 0);
        }

        ESP_LOGI(TAG,
                 "text panel: %d quads placed, %d clipped off right, "
                 "%d shrunk to fit width, %d pinned to bottom, "
                 "scale range=[%.2f, %.2f]× (panel %dx%d)",
                 (int)placed.size(),
                 too_wide,
                 shrunk_for_width,
                 (int)pinned,
                 (placed.empty() ? 1.0 : min_scale_256 / 256.0),
                 (placed.empty() ? 1.0 : max_scale_256 / 256.0),
                 (int)panel_w,
                 (int)panel_h);
    }
    xSemaphoreGive(m_res_mutex);
    lv_obj_invalidate(m_canvas);
    bsp_display_unlock();
}

void WhoPPOCRV6ResultLCDDisp::draw_overlay_locked()
{
    if (m_result.empty() || !m_stable_buf) {
        return;
    }

    const int fb_w = static_cast<int>(m_stable_w);
    const int fb_h = static_cast<int>(m_stable_h);
    const float sx = static_cast<float>(m_stable_w) / static_cast<float>(m_ocr_w);
    const float sy = static_cast<float>(m_stable_h) / static_cast<float>(m_ocr_h);

    lv_layer_t layer;
    lv_canvas_init_layer(m_canvas, &layer);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.width = 2;
    line_dsc.opa = LV_OPA_COVER;

    for (size_t i = 0; i < m_result.size(); ++i) {
        const auto &r = m_result[i];
        line_dsc.color = m_palette[i % m_palette.size()];

        lv_point_precise_t pts[4];
        for (int k = 0; k < 4; ++k) {
            int x = static_cast<int>(std::lround(r.box.points[2 * k] * sx));
            int y = static_cast<int>(std::lround(r.box.points[2 * k + 1] * sy));
            x = std::clamp(x, 0, fb_w - 1);
            y = std::clamp(y, 0, fb_h - 1);
            pts[k].x = x;
            pts[k].y = y;
        }
        for (int k = 0; k < 4; ++k) {
            line_dsc.p1 = pts[k];
            line_dsc.p2 = pts[(k + 1) % 4];
            lv_draw_line(&layer, &line_dsc);
        }
    }

    lv_canvas_finish_layer(m_canvas, &layer);
}

void WhoPPOCRV6ResultLCDDisp::lcd_disp_cb(VideoCapture::Frame * /*fb*/)
{
    // No-op: canvas is refreshed inside save_result(), not per camera frame.
}

void WhoPPOCRV6ResultLCDDisp::show_preview(VideoCapture::Frame *fb)
{
    if (!m_task->is_active() || !m_canvas || !m_stable_buf || !fb) {
        return;
    }

    dl::image::img_t src_img = frame2img(fb);
    if (src_img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB888 &&
        src_img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565LE) {
        return;
    }

    m_transformer.set_src_img(src_img).set_dst_img(m_stable_img).transform();

    bsp_display_lock(0);
    if (m_text_panel) {
        lv_obj_add_flag(m_text_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(m_text_panel);
    }
    lv_obj_invalidate(m_canvas);
    bsp_display_unlock();
}

void WhoPPOCRV6ResultLCDDisp::prepare_for_ocr()
{
    // No-op: backing is kept resident so the last OCR frame stays visible
    // across runs. Retained for source-compat with the app-layer wiring.
}

void WhoPPOCRV6ResultLCDDisp::cleanup()
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    m_result.clear();
    if (m_stable_buf) {
        std::memset(m_stable_buf, 0, static_cast<size_t>(m_stable_w) * m_stable_h * 2);
    }
    xSemaphoreGive(m_res_mutex);
    bsp_display_lock(0);
    if (m_text_panel) {
        lv_obj_clean(m_text_panel);
    }
    if (m_canvas) {
        lv_obj_invalidate(m_canvas);
    }
    bsp_display_unlock();
}

} // namespace lcd_disp
} // namespace who
#endif
