#include "who_pp_ocr_v6.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cinttypes>

static const char *TAG = "WhoPPOCRV6";

namespace who {
namespace ocr {
WhoPPOCRV6::WhoPPOCRV6(const std::string &name,
                       frame_cap::WhoFrameCapNode *frame_cap_node,
                       uint16_t ocr_input_w,
                       uint16_t ocr_input_h) :
    task::WhoTask(name), m_frame_cap_node(frame_cap_node), m_ocr(nullptr), m_rgb888_img({})
{
    frame_cap_node->add_new_frame_signal_subscriber(this);

    // Grab the ~1.92 MB RGB888 working buffer BEFORE constructing PPOCRV6,
    // so model loads (which fragment PSRAM in DUAL mode) don't steal the
    // last contiguous block.
    const size_t buf_size = static_cast<size_t>(ocr_input_w) * ocr_input_h * 3;
    void *buf = heap_caps_aligned_alloc(16, buf_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        buf = heap_caps_aligned_alloc(16, buf_size, MALLOC_CAP_DEFAULT);
    }
    assert(buf);
    m_rgb888_img = {
        .data = buf, .width = ocr_input_w, .height = ocr_input_h, .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888};

    m_ocr = new pp_ocr_v6::PPOCRV6();
}

WhoPPOCRV6::~WhoPPOCRV6()
{
    if (m_rgb888_img.data) {
        heap_caps_free(m_rgb888_img.data);
    }
    delete m_ocr;
}

void WhoPPOCRV6::set_result_cb(const std::function<void(const result_t &)> &result_cb)
{
    m_result_cb = result_cb;
}

void WhoPPOCRV6::set_cleanup_func(const std::function<void()> &cleanup_func)
{
    m_cleanup = cleanup_func;
}

void WhoPPOCRV6::set_run_scope_cbs(const std::function<void()> &pre_run_cb, const std::function<void()> &post_run_cb)
{
    m_pre_run_cb = pre_run_cb;
    m_post_run_cb = post_run_cb;
}

void WhoPPOCRV6::set_preview_cb(const std::function<void(VideoCapture::Frame *)> &preview_cb)
{
    m_preview_cb = preview_cb;
}

void WhoPPOCRV6::set_mode(Mode m)
{
    m_mode.store(m, std::memory_order_release);
}

WhoPPOCRV6::Mode WhoPPOCRV6::get_mode() const
{
    return m_mode.load(std::memory_order_acquire);
}

void WhoPPOCRV6::set_rec_score_threshold(float v)
{
    m_ocr->set_rec_score_threshold(v);
}

float WhoPPOCRV6::get_rec_score_threshold() const
{
    return m_ocr->get_rec_score_threshold();
}

void WhoPPOCRV6::task()
{
    while (true) {
        EventBits_t event_bits =
            xEventGroupWaitBits(m_event_group, NEW_FRAME | TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
        if (event_bits & TASK_STOP) {
            break;
        } else if (event_bits & TASK_PAUSE) {
            xEventGroupSetBits(m_event_group, TASK_PAUSED);
            EventBits_t pause_event_bits =
                xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
            if (pause_event_bits & TASK_STOP) {
                break;
            } else {
                continue;
            }
        }

        auto fb = m_frame_cap_node->cam_fb_peek();
        if (!fb) {
            continue;
        }

        const Mode mode = m_mode.load(std::memory_order_acquire);

        if (mode == Mode::PREVIEW) {
            if (m_preview_cb) {
                m_preview_cb(fb);
            }
            continue;
        }
        if (mode == Mode::RESULT) {
            continue;
        }

        dl::image::img_t src_img = frame2img(fb);
        if (src_img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB888 &&
            src_img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565LE) {
            ESP_LOGE(TAG,
                     "Unsupported camera fb pixel format for OCR: v4l2=0x%08" PRIx32 " dl=%d",
                     fb->pixel_format,
                     static_cast<int>(src_img.pix_type));
            continue;
        }

        m_image_transformer.set_src_img(src_img).set_dst_img(m_rgb888_img).transform();

        if (m_pre_run_cb) {
            m_pre_run_cb();
        }
        auto results = m_ocr->run(m_rgb888_img);
        if (m_post_run_cb) {
            m_post_run_cb();
        }

        // Park in RESULT before firing the callback so UI observers see the
        // finalized mode.
        if (mode == Mode::OCR_PENDING) {
            m_mode.store(Mode::RESULT, std::memory_order_release);
        }

        if (m_result_cb) {
            m_result_cb(results);
        }
    }
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}

void WhoPPOCRV6::cleanup()
{
    if (m_cleanup) {
        m_cleanup();
    }
}
} // namespace ocr
} // namespace who
