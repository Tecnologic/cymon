#pragma once
// Logging helpers that wrap ESP_LOGx with per-component compile-time tags.
// On Linux host builds, esp_log.h is provided as a stub in tests/stubs/.

#include "esp_log.h"  // NOLINT(build/include_subdir) — provided by ESP-IDF or stub

namespace cymon {

// Components register a tag as a constexpr string literal.
// Usage:
//   static constexpr const char* kTag = "CYMON.CAN";
//   CYMON_LOGI(kTag, "frame received id=0x%08X", frame.id);

}  // namespace cymon

#define CYMON_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define CYMON_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define CYMON_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define CYMON_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
