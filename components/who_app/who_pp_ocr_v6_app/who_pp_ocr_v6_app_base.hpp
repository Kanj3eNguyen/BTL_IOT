#pragma once
#include "who_app.hpp"
#include "who_pp_ocr_v6.hpp"

namespace who {
namespace app {
class WhoPPOCRV6AppBase : public WhoApp {
public:
    WhoPPOCRV6AppBase(frame_cap::WhoFrameCap *frame_cap);

protected:
    frame_cap::WhoFrameCap *m_frame_cap;
    ocr::WhoPPOCRV6 *m_ocr;
};
} // namespace app
} // namespace who
