#pragma once

#include "driver_behavior.hpp"

// Wi-Fi and MQTT run independently of the camera pipeline. A disabled
// configuration leaves the serial video and inference usable.
void driver_telemetry_start();
void driver_telemetry_update(const DriverBehavior &behavior);
bool driver_telemetry_connected();
bool driver_telemetry_wifi_connected();
bool driver_telemetry_time_synced();
int driver_telemetry_tls_error();
int driver_telemetry_tls_stack_error();
uint32_t driver_telemetry_wifi_disconnect_reason();
uint32_t driver_telemetry_acked_count();
uint32_t driver_telemetry_sequence();
bool driver_telemetry_guard_enabled();
bool driver_telemetry_guard_ready();
uint32_t driver_telemetry_guard_forwarded_sequence();
uint32_t driver_telemetry_guard_forwarded_count();
