#pragma once

#include "who_detect.hpp"

// COM4 packet format expected by D:\testiot.py: magic, little-endian length, JPEG.
void serial_jpeg_stream_init();
void serial_jpeg_stream_send(const who::detect::WhoDetect::result_t &result);
