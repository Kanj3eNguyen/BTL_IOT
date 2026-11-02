#pragma once

#include "driver_behavior.hpp"

// Wi-Fi and MQTT run independently of the camera pipeline. A disabled
// configuration leaves the serial video and inference usable.
void driver_telemetry_start();
void driver_telemetry_update(const DriverBehavior &behavior);
bool driver_telemetry_connected();
bool driver_telemetry_wifi_connected();
uint32_t driver_telemetry_wifi_disconnect_reason();
uint32_t driver_telemetry_acked_count();
uint32_t driver_telemetry_sequence();
