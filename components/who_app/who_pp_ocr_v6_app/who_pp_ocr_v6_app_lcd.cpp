#include "who_pp_ocr_v6_app_lcd.hpp"
#include "sdkconfig.h"
#include "who_yield2idle.hpp"

#if !BSP_CONFIG_NO_GRAPHIC_LIB
#ifdef CONFIG_PP_OCR_V6_EXAMPLE_HAVE_CJK_FONT
LV_FONT_DECLARE(pp_ocr_v6_cjk_24);
#else
LV_FONT_DECLARE(montserrat_bold_20);
#endif
#endif

namespace who {
namespace app {

namespace detail {
WhoPPOCRV6AppLCDInit::WhoPPOCRV6AppLCDInit(frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node) :
    m_lcd_disp(new lcd_disp::WhoFrameLCDDisp("LCDDisp", lcd_disp_frame_cap_node))
{
}
} // namespace detail

#if !BSP_CONFIG_NO_GRAPHIC_LIB
// Single base font — save_result() applies per-label transform_scale.
static const lv_font_t *ocr_overlay_font_for_quad_h(int /*quad_h_ocr_px*/)
{
#ifdef CONFIG_PP_OCR_V6_EXAMPLE_HAVE_CJK_FONT
    return &pp_ocr_v6_cjk_24;
#else
    return &montserrat_bold_20;
#endif
}

static const lv_font_t *ocr_overlay_font_default()
{
    return ocr_overlay_font_for_quad_h(0);
}
#endif

WhoPPOCRV6AppLCD::WhoPPOCRV6AppLCD(frame_cap::WhoFrameCap *frame_cap,
                                   frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node) :
    detail::WhoPPOCRV6AppLCDInit(lcd_disp_frame_cap_node ? lcd_disp_frame_cap_node : frame_cap->get_last_node()),
    WhoPPOCRV6AppTerm(frame_cap)
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    ,
    m_result_lcd_disp(nullptr),
    m_toggle_btn(nullptr),
    m_toggle_label(nullptr)
#endif
{
    // m_lcd_disp is registered but its task is never started — the canvas is
    // owned by WhoPPOCRV6ResultLCDDisp and updated on the OCR cadence, not the
    // camera cadence.
    WhoApp::add_task(m_lcd_disp);

#if !BSP_CONFIG_NO_GRAPHIC_LIB
    m_result_lcd_disp = new lcd_disp::WhoPPOCRV6ResultLCDDisp(m_ocr,
                                                              m_lcd_disp->get_canvas(),
                                                              m_ocr->get_input_w(),
                                                              m_ocr->get_input_h(),
                                                              &ocr_overlay_font_for_quad_h,
                                                              ocr_overlay_font_default());
    m_ocr->set_cleanup_func(std::bind(&WhoPPOCRV6AppLCD::cleanup, this));
    m_ocr->set_preview_cb([this](VideoCapture::Frame *fb) { m_result_lcd_disp->show_preview(fb); });

    lv_obj_t *screen = lv_obj_get_parent(m_lcd_disp->get_canvas());
    bsp_display_lock(0);
    m_toggle_btn = lv_button_create(screen);
    lv_obj_set_size(m_toggle_btn, 200, 60);
    lv_obj_align(m_toggle_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(m_toggle_btn, 12, 0);
    lv_obj_set_style_bg_color(m_toggle_btn, lv_color_hex(0x1F77B4), 0);
    lv_obj_add_event_cb(m_toggle_btn, &WhoPPOCRV6AppLCD::on_toggle_btn_evt, LV_EVENT_CLICKED, this);

    m_toggle_label = lv_label_create(m_toggle_btn);
    lv_obj_center(m_toggle_label);
    lv_obj_set_style_text_font(m_toggle_label, ocr_overlay_font_default(), 0);
    lv_obj_set_style_text_color(m_toggle_label, lv_color_hex(0xFFFFFF), 0);
    m_ocr->set_mode(ocr::WhoPPOCRV6::Mode::PREVIEW);
    update_toggle_button_locked();
    bsp_display_unlock();
#endif

    // Pause camera fetch across each OCR run so CPU0/DMA don't evict model
    // weights from L2 cache — measured det:model ~5.5 s → ~2.75 s.
    m_ocr->set_run_scope_cbs(
        [this]() {
            for (const auto &node : m_frame_cap->get_all_nodes()) {
                node->pause();
            }
#if !BSP_CONFIG_NO_GRAPHIC_LIB
            m_result_lcd_disp->prepare_for_ocr();
#endif
        },
        [this]() {
            for (const auto &node : m_frame_cap->get_all_nodes()) {
                node->resume();
            }
        });
}

WhoPPOCRV6AppLCD::~WhoPPOCRV6AppLCD()
{
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    delete m_result_lcd_disp;
#endif
}

bool WhoPPOCRV6AppLCD::run()
{
    bool ret = WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : m_frame_cap->get_all_nodes()) {
        ret &= frame_cap_node->run(4096, 2, 0);
    }
    // m_lcd_disp is intentionally not started; see ctor.
    ret &= m_ocr->run(32768, 2, 1);
    return ret;
}

void WhoPPOCRV6AppLCD::ocr_result_cb(const ocr::WhoPPOCRV6::result_t &result)
{
    WhoPPOCRV6AppTerm::ocr_result_cb(result);
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    m_result_lcd_disp->save_result(result, m_ocr->get_last_input_img());
    if (m_toggle_btn) {
        bsp_display_lock(0);
        update_toggle_button_locked();
        bsp_display_unlock();
    }
#endif
}

#if !BSP_CONFIG_NO_GRAPHIC_LIB
void WhoPPOCRV6AppLCD::on_toggle_btn_evt(lv_event_t *e)
{
    auto *self = static_cast<WhoPPOCRV6AppLCD *>(lv_event_get_user_data(e));
    if (self) {
        self->toggle_mode_from_ui();
    }
}

void WhoPPOCRV6AppLCD::toggle_mode_from_ui()
{
    const auto cur = m_ocr->get_mode();
    if (cur == ocr::WhoPPOCRV6::Mode::OCR_PENDING) {
        return;
    }
    const auto next =
        (cur == ocr::WhoPPOCRV6::Mode::PREVIEW) ? ocr::WhoPPOCRV6::Mode::OCR_PENDING : ocr::WhoPPOCRV6::Mode::PREVIEW;
    m_ocr->set_mode(next);
    bsp_display_lock(0);
    update_toggle_button_locked();
    bsp_display_unlock();
}

void WhoPPOCRV6AppLCD::update_toggle_button_locked()
{
    if (!m_toggle_btn || !m_toggle_label) {
        return;
    }
    switch (m_ocr->get_mode()) {
    case ocr::WhoPPOCRV6::Mode::PREVIEW:
        lv_label_set_text(m_toggle_label, "SCAN");
        lv_obj_set_style_bg_color(m_toggle_btn, lv_color_hex(0x1F77B4), 0);
        lv_obj_clear_state(m_toggle_btn, LV_STATE_DISABLED);
        break;
    case ocr::WhoPPOCRV6::Mode::OCR_PENDING:
        lv_label_set_text(m_toggle_label, "SCANNING...");
        lv_obj_set_style_bg_color(m_toggle_btn, lv_color_hex(0x6C757D), 0);
        lv_obj_add_state(m_toggle_btn, LV_STATE_DISABLED);
        break;
    case ocr::WhoPPOCRV6::Mode::RESULT:
        lv_label_set_text(m_toggle_label, "LIVE");
        lv_obj_set_style_bg_color(m_toggle_btn, lv_color_hex(0x59A14F), 0);
        lv_obj_clear_state(m_toggle_btn, LV_STATE_DISABLED);
        break;
    case ocr::WhoPPOCRV6::Mode::CONTINUOUS:
    default:
        lv_label_set_text(m_toggle_label, "AUTO");
        lv_obj_clear_state(m_toggle_btn, LV_STATE_DISABLED);
        break;
    }
}
#endif

void WhoPPOCRV6AppLCD::lcd_disp_cb(VideoCapture::Frame * /*fb*/)
{
    // No-op — LCD-disp task isn't started; kept for the virtual override.
}

void WhoPPOCRV6AppLCD::cleanup()
{
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    m_result_lcd_disp->cleanup();
#endif
}
} // namespace app
} // namespace who
