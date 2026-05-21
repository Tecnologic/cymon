#pragma once

#include <cstdint>

#include "../../include/config/settings.hpp"
#include "esp_err.h"
#include "nvs_flash.h"

namespace cymon {

/// NVS-backed settings manager.
/// Loads and saves the Settings POD struct into the "cymon" NVS namespace.
class NvsSettings {
 public:
  NvsSettings() = default;

  /// Load settings from NVS.  Populates internal cache.
  /// Returns ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if no saved settings.
  esp_err_t Load();

  /// Persist the current settings to NVS.
  esp_err_t Save();

  /// Access the in-memory settings (modifiable).
  [[nodiscard]] Settings& Get() {
    return settings_;
  }
  [[nodiscard]] const Settings& Get() const {
    return settings_;
  }

  /// Reset settings to compile-time defaults and save.
  esp_err_t ResetToDefaults();

 private:
  static constexpr const char* kNvsNamespace = "cymon";
  static constexpr const char* kNvsKey = "settings";

  Settings settings_{};
};

/// Global singleton accessor (initialised in app_main before tasks start).
NvsSettings& GlobalSettings();

}  // namespace cymon
