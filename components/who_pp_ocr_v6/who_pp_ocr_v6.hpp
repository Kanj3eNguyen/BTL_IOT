#pragma once
#include "dl_image_process.hpp"
#include "pp_ocr_v6.hpp"
#include "who_frame_cap.hpp"
#include <atomic>
#include <functional>
#include <vector>

namespace who {
namespace ocr {
class WhoPPOCRV6 : public task::WhoTask {
public:
    static inline constexpr EventBits_t NEW_FRAME = frame_cap::WhoFrameCapNode::NEW_FRAME;

    using result_t = std::vector<pp_ocr_v6::OCRResult>;

    // OCR task modes.
    //   CONTINUOUS  — run OCR on every frame (terminal app default).
    //   PREVIEW     — no OCR; frames go to `set_preview_cb`.
    //   OCR_PENDING — one-shot; runs OCR on the next frame, then flips
    //                 itself to RESULT.
    //   RESULT      — idle; frames are dropped.
    enum class Mode {
        CONTINUOUS = 0,
        PREVIEW = 1,
        OCR_PENDING = 2,
        RESULT = 3,
    };

    // Default 800×800: pp_ocr_v6 det letterboxes to 736×736 with a single
    // 0.92× scale, no aspect distortion.
    WhoPPOCRV6(const std::string &name,
               frame_cap::WhoFrameCapNode *frame_cap_node,
               uint16_t ocr_input_w = 800,
               uint16_t ocr_input_h = 800);
    ~WhoPPOCRV6();

    void set_result_cb(const std::function<void(const result_t &)> &result_cb);
    void set_cleanup_func(const std::function<void()> &cleanup_func);
    // Callback for PREVIEW mode. The fb pointer is only valid during the call.
    void set_preview_cb(const std::function<void(VideoCapture::Frame *)> &preview_cb);

    void set_mode(Mode m);
    Mode get_mode() const;

    // Called before/after every `PPOCRV6::run()`. The LCD app uses these to
    // pause camera fetch during compute, roughly halving det:model time.
    void set_run_scope_cbs(const std::function<void()> &pre_run_cb, const std::function<void()> &post_run_cb);

    void set_rec_score_threshold(float v);
    float get_rec_score_threshold() const;

    // Coordinate frame of `OCRResult::box.points`.
    uint16_t get_input_w() const { return m_rgb888_img.width; }
    uint16_t get_input_h() const { return m_rgb888_img.height; }

    // Working buffer last fed to `PPOCRV6::run()`. Valid during the result
    // callback synchronously invoked from the OCR task after `run()`.
    const dl::image::img_t &get_last_input_img() const { return m_rgb888_img; }

private:
    void task() override;
    void cleanup() override;

    frame_cap::WhoFrameCapNode *m_frame_cap_node;
    pp_ocr_v6::PPOCRV6 *m_ocr;
    dl::image::ImageTransformer m_image_transformer;
    dl::image::img_t m_rgb888_img;
    std::function<void(const result_t &)> m_result_cb;
    std::function<void()> m_cleanup;
    std::function<void()> m_pre_run_cb;
    std::function<void()> m_post_run_cb;
    std::function<void(VideoCapture::Frame *)> m_preview_cb;
    std::atomic<Mode> m_mode{Mode::CONTINUOUS};
};
} // namespace ocr
} // namespace who
