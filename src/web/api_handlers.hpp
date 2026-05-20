#pragma once

#include <esp_http_server.h>

// Free functions implementing the REST API handlers.
// Called from WebServer's static handler methods.

namespace cymon {

esp_err_t ApiHandleGetNodes(httpd_req_t* req);
esp_err_t ApiHandlePostSession(httpd_req_t* req);
esp_err_t ApiHandleDeleteSession(httpd_req_t* req);
esp_err_t ApiHandlePostWifi(httpd_req_t* req);
esp_err_t ApiHandlePostCan(httpd_req_t* req);
esp_err_t ApiHandleGetSettings(httpd_req_t* req);

}  // namespace cymon
