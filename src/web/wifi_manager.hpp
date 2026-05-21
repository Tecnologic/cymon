#pragma once

#include <functional>
#include <string_view>

#include "config/settings.hpp"

namespace cymon {

/// WiFi STA connection with AP fallback.
///
/// Behaviour:
///   1. Tries to connect to the stored SSID (up to kMaxRetries).
///   2. On failure, brings up a SoftAP with SSID "cymon-setup" so the
///      user can configure credentials via the web UI.
///   3. Retries STA connection every kRetryIntervalUs.
class WifiManager {
 public:
  static constexpr const char* kApSsid = "cymon-setup";
  static constexpr const char* kApPassword = "cymon1234";
  static constexpr int kMaxRetries = 5;
  static constexpr uint64_t kRetryIntervalUs = 30000000u;  // 30 s

  using ConnectedCallback = std::function<void(bool connected, std::string_view ip)>;

  explicit WifiManager(ConnectedCallback on_state_change = nullptr);
  ~WifiManager();

  /// Initialise the WiFi driver and attempt STA connection.
  void Init(std::string_view ssid, std::string_view password);

  /// Reconfigure credentials (persists them) and reconnect.
  void Reconnect(std::string_view ssid, std::string_view password);

  [[nodiscard]] bool IsConnected() const {
    return connected_;
  }
  [[nodiscard]] bool IsApMode() const {
    return ap_mode_;
  }
  [[nodiscard]] const char* Ip() const {
    return ip_str_;
  }

 private:
  static void EventHandler(void* arg, int32_t event_id, void* event_data);
  void StartAp();

  ConnectedCallback on_state_change_;
  bool connected_{false};
  bool ap_mode_{false};
  int retry_count_{0};
  char ip_str_[16]{};
  char ssid_[Settings::kSsidMaxLen + 1]{};
  char password_[Settings::kPassMaxLen + 1]{};
};

}  // namespace cymon
