#pragma once
#include "dl_image_process.hpp"
#include "who_pp_ocr_v6.hpp"
#include "who_task.hpp"
#include "bsp/esp-bsp.h"

#if !BSP_CONFIG_NO_GRAPHIC_LIB
#include "lvgl.h"
#include <vector>

namespace who {
namespace lcd_disp {

// Renders PP-OCRv6 results in a two-pane layout: left canvas shows the
// downscaled OCR-input frame + colored quads; right black panel holds one
// LVGL label per line, continuously scaled (`transform_scale_x/y`) so its
// on-screen character size matches the quad on the left.
class WhoPPOCRV6ResultLCDDisp {
public:
    using FontPicker = const lv_font_t *(*)(int quad_h_ocr_px);

    WhoPPOCRV6ResultLCDDisp(task::WhoTask *task,
                            lv_obj_t *canvas,
                            uint16_t ocr_input_w,
                            uint16_t ocr_input_h,
                            FontPicker font_picker,
                            const lv_font_t *default_font);
    ~WhoPPOCRV6ResultLCDDisp();

    // Snapshot `ocr_input_rgb888` into the RGB565 backing and redraw the
    // overlay. The LCD then holds this frame until the next call.
    void save_result(const ocr::WhoPPOCRV6::result_t &result, const dl::image::img_t &ocr_input_rgb888);
    void show_preview(VideoCapture::Frame *fb);

    void lcd_disp_cb(VideoCapture::Frame *fb);
    void prepare_for_ocr();
    void cleanup();

private:
    static std::vector<lv_color_t> default_palette();
    void draw_overlay_locked(); // requires m_res_mutex held

    // Min largest-free PSRAM block preserved for pp_ocr_v6 runtime allocs.
    // Sized for the DUAL rec_s16_w640 worst case (48×640×3 ≈ 92 KB crop
    // buffer per box) with 2× slack.
    static constexpr size_t k_min_headroom_bytes = 200 * 1024;

    task::WhoTask *m_task;
    lv_obj_t *m_canvas;
    uint16_t m_ocr_w;
    uint16_t m_ocr_h;
    FontPicker m_font_picker;
    const lv_font_t *m_default_font;
    SemaphoreHandle_t m_res_mutex;
    ocr::WhoPPOCRV6::result_t m_result;
    std::vector<lv_color_t> m_palette;

    void *m_stable_buf;
    uint16_t m_stable_w;
    uint16_t m_stable_h;
    dl::image::img_t m_stable_img;
    dl::image::ImageTransformer m_transformer;

    lv_obj_t *m_text_panel;
};

} // namespace lcd_disp
} // namespace who
#endif
