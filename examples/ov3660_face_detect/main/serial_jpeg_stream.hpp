#pragma once

#include "who_detect.hpp"

// Submit latest preview without blocking inference on JPEG compression/UART.
// The viewer receives checksummed JSON and JPEG packets over UART0.
void serial_jpeg_stream_init();
bool serial_jpeg_stream_validate_frame(const dl::image::img_t &image);
void serial_jpeg_stream_send(const who::detect::WhoDetect::result_t &result);
