#include "web/wifi_manager.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "logging/logger.hpp"
#include "lwip/ip4_addr.h"

namespace cymon {

static constexpr const char* kTag = "CYMON.WIFI";

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
WifiManager::WifiManager(ConnectedCallback on_state_change) : on_state_change_(std::move(on_state_change)) {}

WifiManager::~WifiManager() {
  esp_wifi_stop();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void WifiManager::Init(std::string_view ssid, std::string_view password) {
  std::strncpy(ssid_, ssid.data(), Settings::kSsidMaxLen);
  std::strncpy(password_, password.data(), Settings::kPassMaxLen);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, EventHandler, this, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, EventHandler, this, nullptr);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  wifi_config_t wifi_cfg{};
  std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), ssid_, sizeof(wifi_cfg.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), password_, sizeof(wifi_cfg.sta.password) - 1);
  wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  CYMON_LOGI(kTag, "WiFi STA started, connecting to %s", ssid_);
}

// ---------------------------------------------------------------------------
// Reconnect
// ---------------------------------------------------------------------------
void WifiManager::Reconnect(std::string_view ssid, std::string_view password) {
  std::strncpy(ssid_, ssid.data(), Settings::kSsidMaxLen);
  std::strncpy(password_, password.data(), Settings::kPassMaxLen);
  retry_count_ = 0;
  connected_ = false;
  ap_mode_ = false;
  esp_wifi_disconnect();

  wifi_config_t wifi_cfg{};
  std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), ssid_, sizeof(wifi_cfg.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), password_, sizeof(wifi_cfg.sta.password) - 1);
  wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
  esp_wifi_connect();
}

// ---------------------------------------------------------------------------
// StartAp
// ---------------------------------------------------------------------------
void WifiManager::StartAp() {
  ap_mode_ = true;
  CYMON_LOGW(kTag, "Starting fallback AP: %s", kApSsid);

  esp_netif_create_default_wifi_ap();
  esp_wifi_set_mode(WIFI_MODE_APSTA);

  wifi_config_t ap_cfg{};
  std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), kApSsid, sizeof(ap_cfg.ap.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), kApPassword, sizeof(ap_cfg.ap.password) - 1);
  ap_cfg.ap.max_connection = 4;
  ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(kApSsid));
  esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

  if (on_state_change_) {
    on_state_change_(false, "192.168.4.1");
  }
}

// ---------------------------------------------------------------------------
// EventHandler
// ---------------------------------------------------------------------------
void WifiManager::EventHandler(void* arg, int32_t event_id, void* event_data) {
  auto* self = static_cast<WifiManager*>(arg);

  if (event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    self->connected_ = false;
    if (self->retry_count_ < kMaxRetries) {
      ++self->retry_count_;
      CYMON_LOGW(kTag, "WiFi disconnected, retry %d/%d", self->retry_count_, kMaxRetries);
      esp_wifi_connect();
    } else {
      self->StartAp();
    }
  } else if (event_id == IP_EVENT_STA_GOT_IP) {
    const auto* evt = static_cast<const ip_event_got_ip_t*>(event_data);
    esp_ip4addr_ntoa(&evt->ip_info.ip, self->ip_str_, sizeof(self->ip_str_));
    self->connected_ = true;
    self->ap_mode_ = false;
    self->retry_count_ = 0;
    CYMON_LOGI(kTag, "Got IP: %s", self->ip_str_);

    // Start SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    if (self->on_state_change_) {
      self->on_state_change_(true, self->ip_str_);
    }
  }
}

}  // namespace cymon
