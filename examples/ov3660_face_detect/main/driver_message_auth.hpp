#pragma once
#include <cstddef>
#include <cstdint>

bool driver_message_auth_init();
void driver_message_auth_connected();
void driver_message_auth_disconnected();
bool driver_message_auth_ready();
bool driver_message_auth_hello(char *wire, size_t capacity, size_t &length);
bool driver_message_auth_sample(const char *data, size_t data_length, uint32_t sequence,
                                uint32_t sample_age_ms, char *wire, size_t capacity, size_t &length);
// True only for a new authenticated ThingsBoard forwarding ACK.
bool driver_message_auth_receive(const char *wire, size_t length);
uint32_t driver_message_auth_forwarded_sequence();
uint32_t driver_message_auth_forwarded_count();
