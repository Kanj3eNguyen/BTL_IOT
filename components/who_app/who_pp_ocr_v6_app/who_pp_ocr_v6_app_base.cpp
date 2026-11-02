#include "who_pp_ocr_v6_app_base.hpp"

namespace who {
namespace app {
WhoPPOCRV6AppBase::WhoPPOCRV6AppBase(frame_cap::WhoFrameCap *frame_cap) :
    m_frame_cap(frame_cap), m_ocr(new ocr::WhoPPOCRV6("PPOCRV6", m_frame_cap->get_last_node()))
{
    WhoApp::add_task_group(m_frame_cap);
    WhoApp::add_task(m_ocr);
}
} // namespace app
} // namespace who
