#include "who_pp_ocr_v6_app_term.hpp"
#include "esp_log.h"
#include "who_yield2idle.hpp"

static const char *TAG = "PPOCRV6";

namespace who {
namespace app {
WhoPPOCRV6AppTerm::WhoPPOCRV6AppTerm(frame_cap::WhoFrameCap *frame_cap) : WhoPPOCRV6AppBase(frame_cap)
{
    m_ocr->set_result_cb(std::bind(&WhoPPOCRV6AppTerm::ocr_result_cb, this, std::placeholders::_1));
}

bool WhoPPOCRV6AppTerm::run()
{
    bool ret = WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : m_frame_cap->get_all_nodes()) {
        ret &= frame_cap_node->run(4096, 2, 0);
    }
    // Det + rec need a comfortable stack; PP-OCRv6 internals allocate matrices
    // on-stack for perspective transforms.
    ret &= m_ocr->run(32768, 2, 1);
    return ret;
}

void WhoPPOCRV6AppTerm::ocr_result_cb(const ocr::WhoPPOCRV6::result_t &result)
{
    ESP_LOGI(TAG, "OCR results: %u", static_cast<unsigned>(result.size()));
    for (const auto &res : result) {
        ESP_LOGI(TAG,
                 "text=\"%s\", score=%.4f, box=[%d,%d %d,%d %d,%d %d,%d], det_score=%.4f",
                 res.text.c_str(),
                 res.score,
                 res.box.points[0],
                 res.box.points[1],
                 res.box.points[2],
                 res.box.points[3],
                 res.box.points[4],
                 res.box.points[5],
                 res.box.points[6],
                 res.box.points[7],
                 res.box.score);
    }
}
} // namespace app
} // namespace who
