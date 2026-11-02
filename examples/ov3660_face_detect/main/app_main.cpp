#include "frame_cap_pipeline.hpp"
#include "who_detect_app_lcd.hpp"
#include "who_detect_app_term.hpp"
#include "serial_jpeg_stream.hpp"
#include "driver_telemetry.hpp"
#include "driver_status_led.hpp"
#include "who_yield2idle.hpp"
#include "bsp/esp-bsp.h"
#if defined(CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION)
#include "human_face_detect.hpp"
#elif defined(CONFIG_PEDESTRIAN_DETECT_MODEL_LOCATION)
#include "pedestrian_detect.hpp"
#elif defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
#include "cat_detect.hpp"
#elif defined(CONFIG_DOG_DETECT_MODEL_LOCATION)
#include "dog_detect.hpp"
#endif

using namespace who::frame_cap;
using namespace who::app;

class WhoDetectAppSerial : public WhoDetectAppTerm {
public:
    explicit WhoDetectAppSerial(WhoFrameCap *frame_cap) : WhoDetectAppTerm(frame_cap)
    {
        m_detect->set_detect_result_cb([](const who::detect::WhoDetect::result_t &result) {
            serial_jpeg_stream_send(result);
        });
    }

    bool run() override
    {
        bool ok = who::WhoYield2Idle::get_instance()->run();
        for (const auto &node : m_frame_cap->get_all_nodes()) {
            ok &= node->run(4096, 2, 0);
        }
        // JPEG conversion, telemetry formatting and face analysis run inside
        // this callback, so the original 2560-byte Detect stack is too small.
        ok &= m_detect->run(8192, 2, 1);
        return ok;
    }
};

dl::detect::Detect *get_detect_model()
{
#if defined(CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION)
    return new HumanFaceDetect(static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL),
                               false);
#elif defined(CONFIG_PEDESTRIAN_DETECT_MODEL_LOCATION)
    return new PedestrianDetect(static_cast<PedestrianDetect::model_type_t>(CONFIG_DEFAULT_PEDESTRIAN_DETECT_MODEL),
                                false);
#elif defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
    return new CatDetect(static_cast<CatDetect::model_type_t>(CONFIG_DEFAULT_CAT_DETECT_MODEL), false);
#elif defined(CONFIG_DOG_DETECT_MODEL_LOCATION)
    return new DogDetect(static_cast<DogDetect::model_type_t>(CONFIG_DEFAULT_DOG_DETECT_MODEL), false);
#else
    ESP_LOGE("MAIN", "No detect model component in idf_component.yml");
    return nullptr;
#endif
}

void run_detect_lcd()
{
    WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr;
#if CONFIG_IDF_TARGET_ESP32S3
    auto frame_cap = get_dvp_frame_cap_pipeline(true);
#elif CONFIG_IDF_TARGET_ESP32P4
    auto frame_cap = get_mipi_csi_frame_cap_pipeline(true);
    // auto frame_cap = get_uvc_frame_cap_pipeline(true);
#endif
    auto detect_app = new WhoDetectAppLCD({{255, 0, 0}}, frame_cap, lcd_disp_frame_cap_node);
    // create model later to avoid memory fragmentation.
    detect_app->set_model(get_detect_model());
    detect_app->run();
}

void run_detect_term()
{
#if CONFIG_IDF_TARGET_ESP32S3
    auto frame_cap = get_dvp_frame_cap_pipeline(false);
#elif CONFIG_IDF_TARGET_ESP32P4
    auto frame_cap = get_mipi_csi_frame_cap_pipeline(false);
    // auto frame_cap = get_uvc_frame_cap_pipeline(false);
#endif
    auto detect_app = new WhoDetectAppSerial(frame_cap);
    // create model later to avoid memory fragmentation.
    detect_app->set_model(get_detect_model());
    detect_app->run();
}

extern "C" void app_main(void)
{
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);
    serial_jpeg_stream_init();
#if defined(CONFIG_CAMERA_OV3660) && defined(BSP_BOARD_ESP32_S3_EYE)
    driver_status_led_start();
#endif
    driver_telemetry_start();
#if CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD || CONFIG_PEDESTRIAN_DETECT_MODEL_IN_SDCARD || \
    CONFIG_CAT_DETECT_MODEL_IN_SDCARD || CONFIG_DOG_DETECT_MODEL_IN_SDCARD
    ESP_ERROR_CHECK(bsp_sdcard_mount());
#endif

// close led
#if defined(BSP_BOARD_ESP32_S3_EYE) && !defined(CONFIG_CAMERA_OV3660)
    led_indicator_handle_t leds[BSP_LED_NUM];
    ESP_ERROR_CHECK(bsp_led_indicator_create(leds, NULL, BSP_LED_NUM));
    ESP_ERROR_CHECK(bsp_led_set(leds[0], false));
#endif

#if defined(CONFIG_CAMERA_OV3660) && defined(BSP_BOARD_ESP32_S3_EYE)
    run_detect_term();
#else
    run_detect_lcd();
#endif
}
