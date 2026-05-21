#include <cstdint>

#include "can/cyphal_transport.hpp"
#include "can/mcp2518fd.hpp"
#include "config/settings.hpp"
#include "cyphal/allocator.hpp"
#include "cyphal/node.hpp"
#include "cyphal/scanner.hpp"
#include "cyphal/subject_scanner.hpp"
#include "cyphal/timesync.hpp"
#include "cyphal/variable_fetcher.hpp"
#include "esp_efuse.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logging/logger.hpp"
#include "monitor/session_manager.hpp"
#include "nvs_flash.h"
#include "web/api_handlers.hpp"
#include "web/web_server.hpp"
#include "web/wifi_manager.hpp"
#include "web/ws_streamer.hpp"

static constexpr const char* kTag = "CYMON.MAIN";

// ---------------------------------------------------------------------------
// Hardware configuration — adjust to match your PCB
// ---------------------------------------------------------------------------
static constexpr cymon::Mcp2518fd::PinConfig kCanPins{
    .spi_mosi = 11,
    .spi_miso = 13,
    .spi_sck = 12,
    .spi_cs = 10,
    .int_pin = 9,
    .stby_pin = -1,
};

// ---------------------------------------------------------------------------
// Global objects (static storage, initialised in app_main)
// ---------------------------------------------------------------------------
static cymon::NvsSettings g_nvs_settings;
static cymon::SessionManager g_session_manager;

// ---------------------------------------------------------------------------
// Task bodies (forward declarations)
// ---------------------------------------------------------------------------
static void CanRxTaskBody(void* arg);
static void ScannerTaskBody(void* arg);
static void WsStreamerTaskBody(void* arg);

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
extern "C" void app_main() {
  CYMON_LOGI(kTag, "cymon starting");

  // NVS
  ESP_ERROR_CHECK(nvs_flash_init());
  g_nvs_settings.Load();
  const cymon::Settings& cfg = g_nvs_settings.Get();

  // CAN driver
  cymon::Mcp2518fd::BaudConfig baud{cfg.can.nominal_kbps, cfg.can.data_mbps, 40000000u};
  static cymon::Mcp2518fd can_driver(kCanPins, baud);
  if (!can_driver.Init()) {
    CYMON_LOGE(kTag, "CAN driver init failed — halting");
    for (;;)
      vTaskDelay(portMAX_DELAY);
  }

  // Cyphal transport
  static cymon::CyphalTransport transport(cfg.cyphal_node_id);

  // Wire CAN → Cyphal
  transport.SetSendFn([&](const cymon::CanFrame& f) { return can_driver.Transmit(f); });
  can_driver.SetRxCallback([&transport](const cymon::CanFrame& frame) { transport.IngestFrame(frame); });

  // Cyphal node
  cymon::CyphalNode::NodeInfo node_info{};
  // Read 128-bit unique ID from ESP32-S2 eFuse
  esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, node_info.unique_id, 128);
  static cymon::CyphalNode cyphal_node(transport, node_info);

  // Drain TX and spin the node heartbeat from inside the single CAN task.
  // This makes the RX task the sole owner of the SPI bus — no mutex needed.
  // SetTxDrainFn must be called after cyphal_node is constructed.
  can_driver.SetTxDrainFn([&](uint64_t now) {
    cyphal_node.Spin(now);
    transport.Poll();
  });

  // Network scanner
  static cymon::NetworkScanner scanner(transport, [](const cymon::NodeRecord& rec) {
    CYMON_LOGI(kTag, "Node %u changed health=%u", rec.node_id, static_cast<uint8_t>(rec.health));
  });

  // Variable fetcher
  static cymon::VariableFetcher var_fetcher(transport, [](uint8_t node_id, const std::vector<cymon::VariableInfo>& vars) {
    CYMON_LOGI(kTag, "Node %u: %zu variables", node_id, vars.size());
  });

  // Subject scanner
  static cymon::SubjectScanner subj_scanner(transport, [](uint8_t node_id, const std::vector<cymon::SubjectInfo>& subs) {
    CYMON_LOGI(kTag, "Node %u: %zu subjects", node_id, subs.size());
  });

  // PnP allocator — reserve the monitor's own node-ID so it is never handed out
  static cymon::PnpAllocator pnp_allocator(transport, cfg.cyphal_node_id);

  // Time sync
  static cymon::TimeSyncProvider time_sync(transport);

  // WiFi
  static cymon::WifiManager wifi_manager;
  wifi_manager.Init(cfg.wifi_ssid, cfg.wifi_pass);

  // HTTP server
  static cymon::WebServer web_server;
  web_server.Start();

  // WS streamer
  static cymon::WsStreamer ws_streamer(g_session_manager, web_server);

  // Inject globals into API handlers
  cymon::InitApiGlobals(&g_session_manager, &scanner.Nodes(), &wifi_manager, &can_driver, &g_nvs_settings);

  // Create tasks
  // Store pointers needed by tasks in static context
  struct TaskArgs {
    cymon::Mcp2518fd* can;
    cymon::CyphalTransport* transport;
    cymon::CyphalNode* node;
    cymon::NetworkScanner* scanner;
    cymon::VariableFetcher* var_fetcher;
    cymon::SubjectScanner* subj_scanner;
    cymon::PnpAllocator* pnp;
    cymon::TimeSyncProvider* timesync;
    cymon::WsStreamer* ws;
  };
  static TaskArgs args{&can_driver,   &transport,     &cyphal_node, &scanner,    &var_fetcher,
                       &subj_scanner, &pnp_allocator, &time_sync,   &ws_streamer};

  xTaskCreatePinnedToCore(CanRxTaskBody, "can_rx", 4096, &args, 20, nullptr, 1);
  xTaskCreatePinnedToCore(ScannerTaskBody, "scanner", 4096, &args, 10, nullptr, 0);
  xTaskCreatePinnedToCore(WsStreamerTaskBody, "ws_stream", 8192, &args, 8, nullptr, 0);

  // app_main may return — FreeRTOS scheduler is already running
}

// ---------------------------------------------------------------------------
// Task implementations
// ---------------------------------------------------------------------------
static void CanRxTaskBody(void* arg) {
  struct LocalArgs {
    cymon::Mcp2518fd* can;
  };
  auto* a = static_cast<LocalArgs*>(arg);
  a->can->RxTask();  // Never returns
}

static void ScannerTaskBody(void* arg) {
  struct LocalArgs {
    cymon::Mcp2518fd* can;
    cymon::CyphalTransport* transport;
    cymon::CyphalNode* node;
    cymon::NetworkScanner* scanner;
    cymon::VariableFetcher* var_fetcher;
    cymon::SubjectScanner* subj_scanner;
    cymon::PnpAllocator* pnp;
    cymon::TimeSyncProvider* timesync;
  };
  auto* a = static_cast<LocalArgs*>(arg);

  for (;;) {
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    a->scanner->Tick(now);
    a->var_fetcher->Tick(now);
    a->subj_scanner->Tick(now);
    a->pnp->Tick(now);
    a->timesync->Tick(now);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void WsStreamerTaskBody(void* arg) {
  struct LocalArgs {
    cymon::Mcp2518fd* can;
    cymon::CyphalTransport* transport;
    cymon::CyphalNode* node;
    cymon::NetworkScanner* scanner;
    cymon::VariableFetcher* var_fetcher;
    cymon::SubjectScanner* subj_scanner;
    cymon::PnpAllocator* pnp;
    cymon::TimeSyncProvider* timesync;
    cymon::WsStreamer* ws;
  };
  auto* a = static_cast<LocalArgs*>(arg);

  for (;;) {
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    a->ws->Tick(now);
    vTaskDelay(pdMS_TO_TICKS(50));  // 20 Hz tick, 10 Hz actual publish
  }
}
