#include "web/api_handlers.hpp"

#include <cstring>
#include <map>
#include <string>

#include "can/mcp2518fd.hpp"
#include "config/settings.hpp"
#include "logging/logger.hpp"
#include "monitor/monitor_session.hpp"
#include "monitor/node_record.hpp"
#include "monitor/session_manager.hpp"
#include "web/wifi_manager.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.API";

// ---------------------------------------------------------------------------
// Globals injected at startup (set via InitApiGlobals before server starts)
// ---------------------------------------------------------------------------
static SessionManager* g_session_manager = nullptr;
static const std::map<uint8_t, NodeRecord>* g_node_table = nullptr;
static WifiManager* g_wifi_manager = nullptr;
static Mcp2518fd* g_can_driver = nullptr;
static NvsSettings* g_settings = nullptr;

void InitApiGlobals(SessionManager* sm, const std::map<uint8_t, NodeRecord>* nodes, WifiManager* wifi, Mcp2518fd* can,
                    NvsSettings* settings) {
  g_session_manager = sm;
  g_node_table = nodes;
  g_wifi_manager = wifi;
  g_can_driver = can;
  g_settings = settings;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static esp_err_t ReadBody(httpd_req_t* req, std::string& out) {
  out.resize(req->content_len + 1, '\0');
  int ret = httpd_req_recv(req, out.data(), req->content_len);
  if (ret <= 0) {
    return ESP_FAIL;
  }
  out.resize(static_cast<size_t>(ret));
  return ESP_OK;
}

// Minimal JSON helpers — avoids pulling in a JSON library dependency.
// For a production system, cJSON (bundled with ESP-IDF) would be used.
static std::string JsonString(std::string_view key, std::string_view value) {
  return "\"" + std::string(key) + "\":\"" + std::string(value) + "\"";
}
static std::string JsonInt(std::string_view key, int64_t value) {
  return "\"" + std::string(key) + "\":" + std::to_string(value);
}

// ---------------------------------------------------------------------------
// GET /api/nodes
// ---------------------------------------------------------------------------
esp_err_t ApiHandleGetNodes(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");

  std::string body = "[";
  bool first = true;
  if (g_node_table) {
    for (const auto& [id, rec] : *g_node_table) {
      if (!first)
        body += ",";
      first = false;
      body += "{";
      body += JsonInt("id", id) + ",";
      body += JsonString("name", rec.name) + ",";
      body += JsonInt("health", static_cast<int64_t>(rec.health)) + ",";
      body += "\"variables\":[";
      bool fv = true;
      for (const auto& v : rec.variables) {
        if (!fv)
          body += ",";
        fv = false;
        body += "{" + JsonInt("id", v.variable_id) + "," + JsonString("name", v.name) + "," + JsonString("unit", v.unit) + "}";
      }
      body += "]";
      body += "}";
    }
  }
  body += "]";

  return httpd_resp_sendstr(req, body.c_str());
}

// ---------------------------------------------------------------------------
// POST /api/session
// Body: {"channels":[{"node_id":1,"variable_id":10}],"mode":"rolling","depth_ms":500}
// ---------------------------------------------------------------------------
esp_err_t ApiHandlePostSession(httpd_req_t* req) {
  if (!g_session_manager) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not ready");
    return ESP_FAIL;
  }

  std::string body;
  if (ReadBody(req, body) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
    return ESP_FAIL;
  }

  // Minimal parse — extract first node_id and variable_id
  // Production code would use cJSON here.
  std::vector<ChannelConfig> cfgs;

  // Very simple grep for "node_id":N,"variable_id":M patterns
  const char* p = body.c_str();
  while (*p) {
    const char* nid_pos = std::strstr(p, "\"node_id\":");
    const char* vid_pos = std::strstr(p, "\"variable_id\":");
    if (!nid_pos || !vid_pos)
      break;
    const uint8_t nid = static_cast<uint8_t>(std::atoi(nid_pos + 10));
    const uint8_t vid = static_cast<uint8_t>(std::atoi(vid_pos + 14));
    cfgs.push_back(ChannelConfig{{nid, vid}, ""});
    p = vid_pos + 14;
  }

  if (cfgs.empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No channels");
    return ESP_FAIL;
  }

  const SessionMode mode = (std::strstr(body.c_str(), "triggered")) ? SessionMode::kTriggered : SessionMode::kRolling;

  const uint8_t id = g_session_manager->CreateSession(cfgs, mode);
  if (id == 0xFF) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Too many sessions");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  const std::string resp = "{\"session_id\":" + std::to_string(id) + "}";
  return httpd_resp_sendstr(req, resp.c_str());
}

// ---------------------------------------------------------------------------
// DELETE /api/session/:id
// ---------------------------------------------------------------------------
esp_err_t ApiHandleDeleteSession(httpd_req_t* req) {
  // Extract id from URI
  const std::string uri(req->uri);
  const size_t slash = uri.rfind('/');
  if (slash == std::string::npos || slash + 1 >= uri.size()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing session id");
    return ESP_FAIL;
  }
  const uint8_t id = static_cast<uint8_t>(std::stoi(uri.substr(slash + 1)));

  if (!g_session_manager || !g_session_manager->DestroySession(id)) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Session not found");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /api/wifi
// Body: {"ssid":"...","password":"..."}
// ---------------------------------------------------------------------------
esp_err_t ApiHandlePostWifi(httpd_req_t* req) {
  std::string body;
  if (ReadBody(req, body) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
    return ESP_FAIL;
  }

  // Minimal JSON parse for ssid / password
  auto extract = [&](const char* key) -> std::string {
    const std::string search = std::string("\"") + key + "\":\"";
    const char* pos = std::strstr(body.c_str(), search.c_str());
    if (!pos)
      return {};
    pos += search.size();
    const char* end = std::strchr(pos, '"');
    if (!end)
      return {};
    return std::string(pos, static_cast<size_t>(end - pos));
  };

  const std::string ssid = extract("ssid");
  const std::string pass = extract("password");
  if (ssid.empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
    return ESP_FAIL;
  }

  if (g_settings) {
    std::strncpy(g_settings->Get().wifi_ssid, ssid.c_str(), Settings::kSsidMaxLen);
    std::strncpy(g_settings->Get().wifi_pass, pass.c_str(), Settings::kPassMaxLen);
    g_settings->Save();
  }
  if (g_wifi_manager) {
    g_wifi_manager->Reconnect(ssid, pass);
  }

  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /api/can
// Body: {"nominal_kbps":500,"data_mbps":4}
// ---------------------------------------------------------------------------
esp_err_t ApiHandlePostCan(httpd_req_t* req) {
  std::string body;
  if (ReadBody(req, body) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
    return ESP_FAIL;
  }

  const char* n_pos = std::strstr(body.c_str(), "\"nominal_kbps\":");
  const char* d_pos = std::strstr(body.c_str(), "\"data_mbps\":");
  if (!n_pos || !d_pos) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing baud fields");
    return ESP_FAIL;
  }

  Mcp2518fd::BaudConfig bc{};
  bc.nominal_kbps = static_cast<uint32_t>(std::atoi(n_pos + 15));
  bc.data_mbps = static_cast<uint32_t>(std::atoi(d_pos + 12));

  if (g_can_driver) {
    g_can_driver->SetBaud(bc);
  }
  if (g_settings) {
    g_settings->Get().can.nominal_kbps = bc.nominal_kbps;
    g_settings->Get().can.data_mbps = bc.data_mbps;
    g_settings->Save();
  }

  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /api/settings
// ---------------------------------------------------------------------------
esp_err_t ApiHandleGetSettings(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  std::string body = "{";
  if (g_settings) {
    const Settings& s = g_settings->Get();
    body += JsonString("ssid", s.wifi_ssid) + ",";
    body += JsonInt("nominal_kbps", s.can.nominal_kbps) + ",";
    body += JsonInt("data_mbps", s.can.data_mbps) + ",";
    body += JsonInt("cyphal_node_id", s.cyphal_node_id);
  }
  body += "}";
  return httpd_resp_sendstr(req, body.c_str());
}

}  // namespace cymon
