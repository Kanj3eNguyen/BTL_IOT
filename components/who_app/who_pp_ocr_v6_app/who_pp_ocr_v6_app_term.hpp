#pragma once
#include "who_pp_ocr_v6_app_base.hpp"

namespace who {
namespace app {
class WhoPPOCRV6AppTerm : public WhoPPOCRV6AppBase {
public:
    WhoPPOCRV6AppTerm(frame_cap::WhoFrameCap *frame_cap);
    bool run() override;

protected:
    virtual void ocr_result_cb(const ocr::WhoPPOCRV6::result_t &result);
};
} // namespace app
} // namespace who
