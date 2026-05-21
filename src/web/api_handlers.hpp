#pragma once

#include <esp_http_server.h>

#include <cstdint>
#include <map>

// Free functions implementing the REST API handlers.
// Called from WebServer's static handler methods.

namespace cymon {

// Forward declarations for InitApiGlobals parameters.
struct NodeRecord;
class SessionManager;
class WifiManager;
class Mcp2518fd;
class NvsSettings;

/// Inject the module-global pointers that the REST handlers use.
/// Must be called before the HTTP server is started.
void InitApiGlobals(SessionManager* sm, const std::map<uint8_t, NodeRecord>* nodes, WifiManager* wifi, Mcp2518fd* can,
                    NvsSettings* settings);

esp_err_t ApiHandleGetNodes(httpd_req_t* req);
esp_err_t ApiHandlePostSession(httpd_req_t* req);
esp_err_t ApiHandleDeleteSession(httpd_req_t* req);
esp_err_t ApiHandlePostWifi(httpd_req_t* req);
esp_err_t ApiHandlePostCan(httpd_req_t* req);
esp_err_t ApiHandleGetSettings(httpd_req_t* req);

}  // namespace cymon
