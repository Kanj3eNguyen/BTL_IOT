#include "frame_cap_pipeline.hpp"
#include "driver_lighting.hpp"
#include "esp_video_init.h"
#include "video_capture.hpp"
#include "bsp/esp-bsp.h"
#if defined(CONFIG_CAMERA_OV3660) && defined(BSP_BOARD_ESP32_S3_EYE)
#include "esp_cam_sensor_xclk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

using namespace who::frame_cap;

#if defined(CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION)
// num of frames the model take to get result
#define MODEL_TIME 2
#define MODEL_INPUT_W 160
#define MODEL_INPUT_H 120
#elif defined(CONFIG_PEDESTRIAN_DETECT_MODEL_LOCATION)
// num of frames the model take to get result
#define MODEL_TIME 3
#define MODEL_INPUT_W 224
#define MODEL_INPUT_H 224
#elif defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
// num of frames the model take to get result
#if defined(CONFIG_ESPDET_PICO_224_224_CAT)
#define MODEL_TIME 3
#define MODEL_INPUT_W 224
#define MODEL_INPUT_H 224
#endif
#if defined(CONFIG_ESPDET_PICO_416_416_CAT)
#define MODEL_TIME 8
#define MODEL_INPUT_W 416
#define MODEL_INPUT_H 416
#endif
#elif defined(CONFIG_DOG_DETECT_MODEL_LOCATION)
// num of frames the model take to get result
#if defined(CONFIG_ESPDET_PICO_224_224_DOG)
#define MODEL_TIME 3
#define MODEL_INPUT_W 224
#define MODEL_INPUT_H 224
#endif
#if defined(CONFIG_ESPDET_PICO_416_416_DOG)
#define MODEL_TIME 8
#define MODEL_INPUT_W 416
#define MODEL_INPUT_H 416
#endif
#endif

// The size of the fb_count and ringbuf_len must be big enough. If you have no idea how to set them, try with 5 and
// larger.
#if CONFIG_IDF_TARGET_ESP32S3
#if defined(CONFIG_CAMERA_OV3660) && defined(BSP_BOARD_ESP32_S3_EYE)
// The EYE BSP uses a 16 MHz XCLK. The OV3660 RGB565 240x240 mode needs 20 MHz.
static void start_ov3660_camera()
{
    ESP_ERROR_CHECK(bsp_i2c_init());

#if defined(BSP_CAMERA_GPIO_XCLK)
    constexpr gpio_num_t xclk_pin = BSP_CAMERA_GPIO_XCLK;
#else
    constexpr gpio_num_t xclk_pin = BSP_CAMERA_XCLK;
#endif
    esp_cam_sensor_xclk_handle_t xclk_handle = nullptr;
    esp_cam_sensor_xclk_config_t xclk_cfg = {};
    xclk_cfg.ledc_cfg.timer = LEDC_TIMER_1;
    xclk_cfg.ledc_cfg.clk_cfg = LEDC_AUTO_CLK;
#if defined(CONFIG_BSP_CAMERA_XCLK_LEDC_CH)
    xclk_cfg.ledc_cfg.channel = static_cast<ledc_channel_t>(CONFIG_BSP_CAMERA_XCLK_LEDC_CH);
#else
    xclk_cfg.ledc_cfg.channel = LEDC_CHANNEL_0;
#endif
    xclk_cfg.ledc_cfg.xclk_freq_hz = 20000000;
    xclk_cfg.ledc_cfg.xclk_pin = xclk_pin;
    ESP_ERROR_CHECK(esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_LEDC, &xclk_handle));
    ESP_ERROR_CHECK(esp_cam_sensor_xclk_start(xclk_handle, &xclk_cfg));
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_video_init_dvp_config_t dvp_cfg = {};
    dvp_cfg.sccb_config.init_sccb = false;
    dvp_cfg.sccb_config.i2c_handle = bsp_i2c_get_handle();
    dvp_cfg.sccb_config.freq = 400000;
    dvp_cfg.reset_pin = GPIO_NUM_NC;
    dvp_cfg.pwdn_pin = GPIO_NUM_NC;
    dvp_cfg.dvp_pin.data_width = CAM_CTLR_DATA_WIDTH_8;
    const gpio_num_t data_pins[] = {
        BSP_CAMERA_D0, BSP_CAMERA_D1, BSP_CAMERA_D2, BSP_CAMERA_D3,
        BSP_CAMERA_D4, BSP_CAMERA_D5, BSP_CAMERA_D6, BSP_CAMERA_D7,
    };
    for (int i = 0; i < 8; ++i) {
        dvp_cfg.dvp_pin.data_io[i] = data_pins[i];
    }
    dvp_cfg.dvp_pin.vsync_io = BSP_CAMERA_VSYNC;
    dvp_cfg.dvp_pin.de_io = BSP_CAMERA_HSYNC;
    dvp_cfg.dvp_pin.pclk_io = BSP_CAMERA_PCLK;
    // Keep GPIO15 driven by LEDC. The DVP driver must not remap it to its own clock.
    dvp_cfg.dvp_pin.xclk_io = GPIO_NUM_NC;
    dvp_cfg.xclk_freq = 0;

    esp_video_init_config_t video_cfg = {};
    video_cfg.dvp = &dvp_cfg;
    ESP_ERROR_CHECK(esp_video_init(&video_cfg));
}
#endif

WhoFrameCap *get_dvp_frame_cap_pipeline(bool lcd)
{
#if defined(CONFIG_CAMERA_OV3660) && defined(BSP_BOARD_ESP32_S3_EYE)
    start_ov3660_camera();
#else
    bsp_camera_cfg_t cam_cfg{};
    ESP_ERROR_CHECK(bsp_camera_start(&cam_cfg));
#endif
    // The ringbuf_len of FetchNode equals cam_fb_count - 2. The WhoFetchNode fb will display on lcd, if you want to
    // make sure the displayed detection result is synced with the frame, the ringbuf size must be big enough to
    // cover the process time from now to the the detection result is ready. If the ring_buf_len is 3, the frame
    // which the disp task will display is 2 frames before than the frame which feeds into detection task. So the
    // detection task must finish within 2 frames, or the detect result will have a delay compared to the displayed
    // frame.

    auto cap = new VideoCapture();
    auto buffer_cnt = lcd ? (MODEL_TIME + 3) : (MODEL_TIME + 2);
#if defined(CONFIG_CAMERA_OV3660) && defined(BSP_BOARD_ESP32_S3_EYE)
    buffer_cnt = 6; // Keep camera frames valid while detection is processing.
#endif
    auto cfg = VideoCapture::Config(ESP_VIDEO_DVP_DEVICE_NAME, V4L2_PIX_FMT_RGB565X, buffer_cnt);

#if defined(CONFIG_CAMERA_OV3660) && defined(BSP_BOARD_ESP32_S3_EYE)
    // The OV3660 image is upright with VFLIP; mirror it for the driver-facing view.
    cfg.set_hflip(true).set_vflip(true);
#elif defined(BSP_BOARD_ESP32_S3_EYE)
    cfg.set_vflip(true);
#elifdef BSP_BOARD_ESP32_S3_KORVO_2
    cfg.set_hflip(true).set_vflip(true);
#endif
    cap->init(cfg);
    cap->start();
#if defined(CONFIG_CAMERA_OV3660) && defined(BSP_BOARD_ESP32_S3_EYE)
    driver_lighting_init(cap);
#endif

    auto frame_cap = new WhoFrameCap();
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap);
    return frame_cap;
}

#elif CONFIG_IDF_TARGET_ESP32P4
WhoFrameCap *get_mipi_csi_frame_cap_pipeline(bool lcd)
{
    esp_log_level_set("ISP_AWB", ESP_LOG_ERROR);
    bsp_camera_cfg_t cam_cfg{};
    ESP_ERROR_CHECK(bsp_camera_start(&cam_cfg));

    auto cap = new VideoCapture();
    auto buffer_cnt = lcd ? (MODEL_TIME + 3) : (MODEL_TIME + 2);
    auto cfg = VideoCapture::Config(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, V4L2_PIX_FMT_RGB565, buffer_cnt).set_hflip(true);
    cap->init(cfg);
    cap->start();

    auto frame_cap = new WhoFrameCap();
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap);
    return frame_cap;
}

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
WhoFrameCap *get_uvc_frame_cap_pipeline(bool lcd)
{
    esp_video_init_usb_uvc_config_t usb_uvc_cfg = {
        .uvc =
            {
                .uvc_dev_num = 1,
                .task_stack = 4096,
                .task_priority = 10,
                .task_affinity = -1,
            },
        .usb =
            {
                .init_usb_host_lib = true,
                .peripheral_map = 0x00,
                .task_stack = 4096,
                .task_priority = 11,
                .task_affinity = -1,
            },
    };
    esp_video_init_config_t video_cfg = {};
    video_cfg.usb_uvc = &usb_uvc_cfg;
    esp_video_init(&video_cfg);

    auto cap = new VideoCapture();
    auto cfg = VideoCapture::Config(ESP_VIDEO_USB_UVC_NAME(0), V4L2_PIX_FMT_JPEG, 4).set_uvc_config({640, 480, 30});
    cap->init(cfg);
    cap->start();

    auto frame_cap = new WhoFrameCap();
    // The ringbuf_len of FetchNode equals cam_fb_count - 2, the ringbuf_len of FetchNode should take care of the
    // process time of the following Node. For example, if the DecodeNode takes 2 frame to decode, then the
    // FetchNode ringbuf_len is at least 2, and the fb_count of the cam is at least 4.
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap, false);

    if (lcd) {
        // The DecodeNode ringbuf_len relies on the following PPAResizeNode process time, the time of data transfer.
        frame_cap->add_node<WhoDecodeNode>("FrameCapDecode", dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, 2, false);
        // The ppa resized fb will display on lcd, if you want to make sure the displayed detection result is synced
        // with the frame, the ringbuf size must be big enough to cover the process time from now to the the detection
        // result is ready.
        frame_cap->add_node<WhoPPAResizeNode>(
            "FrameCapPPAResize", 800, 600, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, MODEL_TIME + 1);
        return frame_cap;
    } else {
        frame_cap->add_node<WhoDecodeNode>("FrameCapDecode", dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, MODEL_TIME);
        return frame_cap;
    }
}
#endif
#endif
