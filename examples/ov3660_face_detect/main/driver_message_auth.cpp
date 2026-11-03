#include "driver_message_auth.hpp"
#include "sdkconfig.h"

#if CONFIG_DRIVER_MQTT_GUARD_ENABLED
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

namespace {
constexpr char kDomain[] = "DM-MQTT-1\n";
uint8_t key[32];
SemaphoreHandle_t mutex = nullptr;
char boot[33]{};
char request[33]{};
char session[33]{};
int64_t session_expires_ms = 0;
int64_t hello_sent_us = 0;
int64_t request_created_us = 0;
uint32_t highest_sent = 0;
uint32_t forwarded_sequence = 0;
uint32_t forwarded_count = 0;

int64_t now_ms()
{
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

int from_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool identifier(const char *value)
{
    const size_t n = std::strlen(value);
    if (!n || n > 48) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

void new_nonce(char (&out)[33])
{
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (unsigned i = 0; i < sizeof(random); ++i) std::snprintf(out + i * 2, 3, "%02x", random[i]);
}

bool mac_hex(const char *body, size_t length, char (&out)[65])
{
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    const auto *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t mac[32];
    const bool ok = info && mbedtls_md_setup(&context, info, 1) == 0 &&
        mbedtls_md_hmac_starts(&context, key, sizeof(key)) == 0 &&
        mbedtls_md_hmac_update(&context, reinterpret_cast<const uint8_t *>(kDomain), sizeof(kDomain) - 1) == 0 &&
        mbedtls_md_hmac_update(&context, reinterpret_cast<const uint8_t *>(body), length) == 0 &&
        mbedtls_md_hmac_finish(&context, mac) == 0;
    mbedtls_md_free(&context);
    if (ok) {
        for (unsigned i = 0; i < sizeof(mac); ++i) std::snprintf(out + i * 2, 3, "%02x", mac[i]);
    }
    return ok;
}

bool wrap(const char *body, size_t body_length, char *wire, size_t capacity, size_t &length)
{
    char mac[65];
    if (!mac_hex(body, body_length, mac) || capacity < 128) return false;
    const int prefix = std::snprintf(wire, capacity, "{\"v\":1,\"payload\":\"");
    size_t base64_length = 0;
    if (mbedtls_base64_encode(reinterpret_cast<uint8_t *>(wire + prefix), capacity - prefix - 96,
                             &base64_length, reinterpret_cast<const uint8_t *>(body), body_length) != 0) return false;
    const size_t offset = prefix + base64_length;
    const int suffix = std::snprintf(wire + offset, capacity - offset, "\",\"mac\":\"%s\"}", mac);
    if (suffix <= 0 || size_t(suffix) >= capacity - offset) return false;
    length = offset + suffix;
    return true;
}

const char *string_field(cJSON *object, const char *name)
{
    auto *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

int64_t number_field(cJSON *object, const char *name)
{
    auto *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > 9007199254740991.0 || std::floor(item->valuedouble) != item->valuedouble) return -1;
    return static_cast<int64_t>(item->valuedouble);
}

bool is_nonce(const char *value)
{
    if (std::strlen(value) != 32) return false;
    for (unsigned i = 0; i < 32; ++i) if (from_hex(value[i]) < 0) return false;
    return true;
}
} // namespace

bool driver_message_auth_init()
{
    if (!identifier(CONFIG_DRIVER_GUARD_DEVICE_ID) || !identifier(CONFIG_DRIVER_GUARD_KEY_ID) ||
        std::strlen(CONFIG_DRIVER_GUARD_HMAC_KEY_HEX) != 64 || !CONFIG_DRIVER_GUARD_MQTT_PASSWORD[0]) return false;
    for (unsigned i = 0; i < sizeof(key); ++i) {
        const int a = from_hex(CONFIG_DRIVER_GUARD_HMAC_KEY_HEX[i * 2]);
        const int b = from_hex(CONFIG_DRIVER_GUARD_HMAC_KEY_HEX[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        key[i] = (a << 4) | b;
    }
    mutex = xSemaphoreCreateMutex();
    return mutex != nullptr;
}

void driver_message_auth_connected()
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    // Wi-Fi/RF is active here, providing entropy to the hardware RNG.
    if (!boot[0]) new_nonce(boot);
    session[0] = request[0] = '\0';
    session_expires_ms = hello_sent_us = request_created_us = 0;
    forwarded_sequence = 0;
    xSemaphoreGive(mutex);
}

void driver_message_auth_disconnected()
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    session_expires_ms = 0;
    xSemaphoreGive(mutex);
}

bool driver_message_auth_ready()
{
    if (!mutex) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool ready = session[0] && session_expires_ms > now_ms() + 15000;
    xSemaphoreGive(mutex);
    return ready;
}

bool driver_message_auth_hello(char *wire, size_t capacity, size_t &length)
{
    char body[512];
    xSemaphoreTake(mutex, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    if (hello_sent_us && now - hello_sent_us < 3000000) {
        xSemaphoreGive(mutex);
        return false;
    }
    if (!request[0] || now - request_created_us > 15000000) {
        new_nonce(request);
        request_created_us = now;
    }
    const int n = std::snprintf(body, sizeof(body),
        "{\"v\":1,\"type\":\"hello\",\"device_id\":\"%s\",\"key_id\":\"%s\","
        "\"boot_id\":\"%s\",\"request_id\":\"%s\",\"sent_at_ms\":%lld}",
        CONFIG_DRIVER_GUARD_DEVICE_ID, CONFIG_DRIVER_GUARD_KEY_ID, boot, request, now_ms());
    hello_sent_us = now;
    xSemaphoreGive(mutex);
    return n > 0 && size_t(n) < sizeof(body) && wrap(body, n, wire, capacity, length);
}

bool driver_message_auth_sample(const char *data, size_t data_length, uint32_t sequence,
                                uint32_t age, char *wire, size_t capacity, size_t &length)
{
    char body[2048];
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!session[0] || session_expires_ms <= now_ms() + 15000 || age > 15000) {
        xSemaphoreGive(mutex);
        return false;
    }
    const int n = std::snprintf(body, sizeof(body),
        "{\"v\":1,\"type\":\"sample\",\"device_id\":\"%s\",\"key_id\":\"%s\","
        "\"boot_id\":\"%s\",\"session_id\":\"%s\",\"sequence\":%lu,\"sent_at_ms\":%lld,"
        "\"sample_age_ms\":%lu,\"data\":%.*s}",
        CONFIG_DRIVER_GUARD_DEVICE_ID, CONFIG_DRIVER_GUARD_KEY_ID, boot, session,
        static_cast<unsigned long>(sequence), now_ms(), static_cast<unsigned long>(age), int(data_length), data);
    highest_sent = sequence;
    xSemaphoreGive(mutex);
    return n > 0 && size_t(n) < sizeof(body) && wrap(body, n, wire, capacity, length);
}

bool driver_message_auth_receive(const char *wire, size_t length)
{
    if (length > 1536) return false;
    auto *envelope = cJSON_ParseWithLength(wire, length);
    if (!envelope) return false;
    char raw[1025], expected[65];
    size_t raw_length = 0;
    const char *payload = string_field(envelope, "payload");
    const char *mac = string_field(envelope, "mac");
    bool valid = number_field(envelope, "v") == 1 && std::strlen(mac) == 64 &&
        mbedtls_base64_decode(reinterpret_cast<uint8_t *>(raw), sizeof(raw) - 1, &raw_length,
                             reinterpret_cast<const uint8_t *>(payload), std::strlen(payload)) == 0 &&
        mac_hex(raw, raw_length, expected);
    if (valid) {
        volatile uint8_t difference = 0;
        for (unsigned i = 0; i < 64; ++i) difference |= static_cast<uint8_t>(mac[i] ^ expected[i]);
        valid = difference == 0;
    }
    cJSON_Delete(envelope);
    if (!valid) return false;
    raw[raw_length] = '\0';
    if (std::strlen(raw) != raw_length) return false;
    auto *body = cJSON_ParseWithLength(raw, raw_length);
    if (!body) return false;
    const int64_t sent = number_field(body, "sent_at_ms");
    const int64_t now = now_ms();
    valid = number_field(body, "v") == 1 && sent >= now - 30000 && sent <= now + 5000 &&
        !std::strcmp(string_field(body, "device_id"), CONFIG_DRIVER_GUARD_DEVICE_ID) &&
        !std::strcmp(string_field(body, "key_id"), CONFIG_DRIVER_GUARD_KEY_ID);
    bool new_ack = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    valid &= !std::strcmp(string_field(body, "boot_id"), boot);
    const char *type = string_field(body, "type");
    const char *sid = string_field(body, "session_id");
    if (valid && !std::strcmp(type, "session")) {
        const int64_t expires = number_field(body, "expires_at_ms");
        if (!std::strcmp(string_field(body, "request_id"), request) && request[0] && is_nonce(sid) &&
            expires > now + 15000 && expires <= sent + 300000) {
            if (std::strcmp(session, sid)) forwarded_sequence = 0;
            std::strcpy(session, sid);
            session_expires_ms = expires;
        }
    } else if (valid && !std::strcmp(type, "ack") && !std::strcmp(sid, session) &&
               session[0] && session_expires_ms > now && !std::strcmp(string_field(body, "status"), "FORWARDED")) {
        const int64_t sequence = number_field(body, "sequence");
        if (sequence > forwarded_sequence && sequence <= highest_sent) {
            forwarded_sequence = static_cast<uint32_t>(sequence);
            ++forwarded_count;
            new_ack = true;
        }
    }
    xSemaphoreGive(mutex);
    cJSON_Delete(body);
    return new_ack;
}

uint32_t driver_message_auth_forwarded_sequence()
{
    if (!mutex) return 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const auto result = forwarded_sequence;
    xSemaphoreGive(mutex);
    return result;
}

uint32_t driver_message_auth_forwarded_count()
{
    if (!mutex) return 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const auto result = forwarded_count;
    xSemaphoreGive(mutex);
    return result;
}
#else
bool driver_message_auth_init() { return false; }
void driver_message_auth_connected() {}
void driver_message_auth_disconnected() {}
bool driver_message_auth_ready() { return false; }
bool driver_message_auth_hello(char *, size_t, size_t &) { return false; }
bool driver_message_auth_sample(const char *, size_t, uint32_t, uint32_t, char *, size_t, size_t &) { return false; }
bool driver_message_auth_receive(const char *, size_t) { return false; }
uint32_t driver_message_auth_forwarded_sequence() { return 0; }
uint32_t driver_message_auth_forwarded_count() { return 0; }
#endif
