#include "who_qrcode_app_base.hpp"

namespace who {
namespace app {
WhoQRCodeAppBase::WhoQRCodeAppBase(frame_cap::WhoFrameCap *frame_cap, int w, int h) :
    m_frame_cap(frame_cap), m_qrcode(new qrcode::WhoQRCode("QRCode", m_frame_cap->get_last_node(), w, h))
{
    WhoApp::add_task_group(m_frame_cap);
    WhoApp::add_task(m_qrcode);
}
} // namespace app
} // namespace who
