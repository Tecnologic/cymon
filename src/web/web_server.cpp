#include "web/web_server.hpp"

#include <cstring>

#include "esp_spiffs.h"
#include "logging/logger.hpp"
#include "web/api_handlers.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.HTTP";
static constexpr const char* kSpiffsBase = "/spiffs";

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
WebServer::~WebServer() {
  Stop();
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------
bool WebServer::Start() {
  // Mount SPIFFS
  esp_vfs_spiffs_conf_t spiffs_conf{};
  spiffs_conf.base_path = kSpiffsBase;
  spiffs_conf.partition_label = nullptr;
  spiffs_conf.max_files = 8;
  spiffs_conf.format_if_mount_failed = false;

  const esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
  if (ret != ESP_OK) {
    CYMON_LOGE(kTag, "SPIFFS mount failed: %d", ret);
    // Non-fatal — REST API still works, UI assets won't be served
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kPort;
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&server_, &config) != ESP_OK) {
    CYMON_LOGE(kTag, "Failed to start HTTP server");
    return false;
  }

  // Register REST endpoints
  static const httpd_uri_t uris[] = {
      {"/api/nodes", HTTP_GET, HandleGetNodes, nullptr},
      {"/api/session", HTTP_POST, HandlePostSession, nullptr},
      {"/api/session/*", HTTP_DELETE, HandleDeleteSession, nullptr},
      {"/api/wifi", HTTP_POST, HandlePostWifi, nullptr},
      {"/api/can", HTTP_POST, HandlePostCan, nullptr},
      {"/api/settings", HTTP_GET, HandleGetSettings, nullptr},
      {.uri = "/ws",
       .method = HTTP_GET,
       .handler = HandleWebSocket,
       .user_ctx = nullptr,
       .is_websocket = true,
       .handle_ws_control_frames = true},
      {"/*", HTTP_GET, HandleStaticFile, nullptr},  // catch-all for SPIFFS
  };

  for (const auto& uri : uris) {
    httpd_register_uri_handler(server_, &uri);
  }

  CYMON_LOGI(kTag, "HTTP server started on port %u", kPort);
  return true;
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------
void WebServer::Stop() {
  if (server_ != nullptr) {
    httpd_stop(server_);
    server_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// BroadcastWs
// ---------------------------------------------------------------------------
void WebServer::BroadcastWs(const uint8_t* data, size_t len) {
  if (server_ == nullptr) {
    return;
  }
  httpd_ws_frame_t frame{};
  frame.type = HTTPD_WS_TYPE_BINARY;
  frame.payload = const_cast<uint8_t*>(data);
  frame.len = len;

  // Iterate over all open file descriptors and send to WebSocket clients
  int client_fds[8]{};
  size_t clients = std::size(client_fds);
  if (httpd_get_client_list(server_, &clients, client_fds) == ESP_OK) {
    for (size_t i = 0; i < clients; ++i) {
      const httpd_ws_client_info_t info = httpd_ws_get_fd_info(server_, client_fds[i]);
      if (info == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_send_frame_async(server_, client_fds[i], &frame);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// HandleWebSocket
// ---------------------------------------------------------------------------
esp_err_t WebServer::HandleWebSocket(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    CYMON_LOGI(kTag, "WebSocket handshake");
    return ESP_OK;
  }
  // Discard inbound WS frames (control is via REST)
  httpd_ws_frame_t frame{};
  frame.type = HTTPD_WS_TYPE_BINARY;
  uint8_t buf[64]{};
  frame.payload = buf;
  httpd_ws_recv_frame(req, &frame, sizeof(buf));
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// HandleStaticFile — serve from SPIFFS /spiffs/www/<uri>
// ---------------------------------------------------------------------------
esp_err_t WebServer::HandleStaticFile(httpd_req_t* req) {
  char path[128];
  snprintf(path, sizeof(path), "%s/www%s", kSpiffsBase, (std::strcmp(req->uri, "/") == 0) ? "/index.html" : req->uri);

  FILE* f = fopen(path, "r");
  if (f == nullptr) {
    // Fallback to index.html for SPA routing
    snprintf(path, sizeof(path), "%s/www/index.html", kSpiffsBase);
    f = fopen(path, "r");
    if (f == nullptr) {
      httpd_resp_send_404(req);
      return ESP_FAIL;
    }
  }

  // Determine content type
  const char* content_type = "application/octet-stream";
  if (std::strstr(path, ".html"))
    content_type = "text/html";
  else if (std::strstr(path, ".js"))
    content_type = "application/javascript";
  else if (std::strstr(path, ".css"))
    content_type = "text/css";
  else if (std::strstr(path, ".json"))
    content_type = "application/json";
  httpd_resp_set_type(req, content_type);

  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    httpd_resp_send_chunk(req, buf, static_cast<ssize_t>(n));
  }
  fclose(f);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

// Forward the REST handler implementations to api_handlers.cpp
esp_err_t WebServer::HandleGetNodes(httpd_req_t* req) {
  return ApiHandleGetNodes(req);
}
esp_err_t WebServer::HandlePostSession(httpd_req_t* req) {
  return ApiHandlePostSession(req);
}
esp_err_t WebServer::HandleDeleteSession(httpd_req_t* req) {
  return ApiHandleDeleteSession(req);
}
esp_err_t WebServer::HandlePostWifi(httpd_req_t* req) {
  return ApiHandlePostWifi(req);
}
esp_err_t WebServer::HandlePostCan(httpd_req_t* req) {
  return ApiHandlePostCan(req);
}
esp_err_t WebServer::HandleGetSettings(httpd_req_t* req) {
  return ApiHandleGetSettings(req);
}

}  // namespace cymon
