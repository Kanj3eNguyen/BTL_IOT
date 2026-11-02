#pragma once
#include "who_frame_lcd_disp.hpp"
#include "who_pp_ocr_v6_app_term.hpp"
#include "who_pp_ocr_v6_result_lcd_disp.hpp"

namespace who {
namespace app {

namespace detail {
// Helper base that allocates the ~2.4 MB LCD DPI framebuffer BEFORE the OCR
// base class loads its model rodata. Required for DUAL rec mode, where
// SPIRAM_XIP_FROM_PSRAM otherwise fragments PSRAM below the DPI panel's
// contiguous-alloc requirement.
struct WhoPPOCRV6AppLCDInit {
    lcd_disp::WhoFrameLCDDisp *m_lcd_disp;
    explicit WhoPPOCRV6AppLCDInit(frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node);
};
} // namespace detail

class WhoPPOCRV6AppLCD : private detail::WhoPPOCRV6AppLCDInit, public WhoPPOCRV6AppTerm {
public:
    WhoPPOCRV6AppLCD(frame_cap::WhoFrameCap *frame_cap, frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr);
    ~WhoPPOCRV6AppLCD();
    bool run() override;

protected:
    void ocr_result_cb(const ocr::WhoPPOCRV6::result_t &result) override;
    virtual void lcd_disp_cb(VideoCapture::Frame *fb);
    virtual void cleanup();

private:
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    static void on_toggle_btn_evt(lv_event_t *e);
    void toggle_mode_from_ui();
    void update_toggle_button_locked();

    lcd_disp::WhoPPOCRV6ResultLCDDisp *m_result_lcd_disp;
    lv_obj_t *m_toggle_btn;
    lv_obj_t *m_toggle_label;
#endif
};
} // namespace app
} // namespace who
