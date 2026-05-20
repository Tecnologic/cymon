#pragma once

#include <cstdint>

namespace cymon {

/// WiFi operating mode.
enum class WifiMode : uint8_t {
  kStation = 0,    ///< Connect to an AP
  kAccessPoint = 1,  ///< Run as AP (fallback)
};

/// CAN-FD baud rate pair.
struct CanBaud {
  uint32_t nominal_kbps{500};  ///< Nominal / arbitration phase kbit/s
  uint32_t data_mbps{2};       ///< Data phase Mbit/s (CAN-FD BRS)
};

/// Persistent runtime settings stored in NVS.
/// This is a plain-old-data struct; the NvsSettings class serialises it.
struct Settings {
  static constexpr size_t kSsidMaxLen = 32;
  static constexpr size_t kPassMaxLen = 64;

  char wifi_ssid[kSsidMaxLen + 1]{};
  char wifi_pass[kPassMaxLen + 1]{};
  WifiMode wifi_mode{WifiMode::kStation};

  CanBaud can{};

  /// Cyphal local node-ID for the monitor (1–127; 0 = unset).
  uint8_t cyphal_node_id{127};
};

}  // namespace cymon
