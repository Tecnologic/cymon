#pragma once
// Minimal ESP-IDF nvs_flash.h stub for Linux host builds.
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK    0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102

typedef void* nvs_handle_t;

static inline esp_err_t nvs_flash_init(void) { return ESP_OK; }
static inline esp_err_t nvs_open(const char* /*name*/, int /*mode*/, nvs_handle_t* /*out*/) { return ESP_FAIL; }
static inline esp_err_t nvs_commit(nvs_handle_t /*h*/) { return ESP_OK; }
static inline void      nvs_close(nvs_handle_t /*h*/) {}
