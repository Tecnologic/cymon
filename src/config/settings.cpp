#include "config/settings.hpp"

#include "logging/logger.hpp"
#include "nvs.h"

namespace cymon {

static constexpr const char* kTag = "CYMON.CFG";

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
esp_err_t NvsSettings::Load() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    CYMON_LOGI(kTag, "No saved settings found, using defaults");
    return err;
  }

  size_t required_size = sizeof(Settings);
  err = nvs_get_blob(handle, kNvsKey, &settings_, &required_size);
  nvs_close(handle);

  if (err != ESP_OK) {
    CYMON_LOGW(kTag, "Failed to read settings blob: %d", err);
    return err;
  }
  CYMON_LOGI(kTag, "Settings loaded from NVS");
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
esp_err_t NvsSettings::Save() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    CYMON_LOGE(kTag, "Failed to open NVS namespace: %d", err);
    return err;
  }

  err = nvs_set_blob(handle, kNvsKey, &settings_, sizeof(Settings));
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err != ESP_OK) {
    CYMON_LOGE(kTag, "Failed to save settings: %d", err);
  } else {
    CYMON_LOGI(kTag, "Settings saved to NVS");
  }
  return err;
}

// ---------------------------------------------------------------------------
// ResetToDefaults
// ---------------------------------------------------------------------------
esp_err_t NvsSettings::ResetToDefaults() {
  settings_ = Settings{};
  return Save();
}

// ---------------------------------------------------------------------------
// GlobalSettings
// ---------------------------------------------------------------------------
NvsSettings& GlobalSettings() {
  static NvsSettings instance;
  return instance;
}

}  // namespace cymon
