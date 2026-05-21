#pragma once

#include <esp_http_server.h>

#include <functional>

namespace cymon {

/// HTTP + WebSocket server backed by esp_http_server.
/// Serves static files from SPIFFS and dispatches REST + WS handlers.
class WebServer {
 public:
  static constexpr uint16_t kPort = 80;
  static constexpr size_t kMaxOpenSockets = 7;

  WebServer() = default;
  ~WebServer();

  /// Start the HTTP server.  Must be called after WiFi/AP is up.
  bool Start();
  void Stop();

  [[nodiscard]] bool IsRunning() const {
    return server_ != nullptr;
  }

  /// Send a binary WebSocket frame to all connected WS clients.
  void BroadcastWs(const uint8_t* data, size_t len);

 private:
  // REST handlers
  static esp_err_t HandleGetNodes(httpd_req_t* req);
  static esp_err_t HandlePostSession(httpd_req_t* req);
  static esp_err_t HandleDeleteSession(httpd_req_t* req);
  static esp_err_t HandlePostWifi(httpd_req_t* req);
  static esp_err_t HandlePostCan(httpd_req_t* req);
  static esp_err_t HandleGetSettings(httpd_req_t* req);

  // WebSocket handler
  static esp_err_t HandleWebSocket(httpd_req_t* req);

  // SPIFFS static file handler
  static esp_err_t HandleStaticFile(httpd_req_t* req);

  httpd_handle_t server_{nullptr};
};

}  // namespace cymon
