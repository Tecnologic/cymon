#include "can/mcp2518fd.hpp"

#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace cymon {

static constexpr const char* kTag = "CYMON.MCP";

// SPI command word encoding (MCP2518FD datasheet §3.1)
static constexpr uint16_t kSpiCmdReset = 0x0000u;
static constexpr uint8_t kSpiCmdRead = 0x03u;
static constexpr uint8_t kSpiCmdWrite = 0x02u;

static TaskHandle_t s_rx_task_handle = nullptr;

// ---------------------------------------------------------------------------
// ISR — called when INT pin goes low
// ---------------------------------------------------------------------------
static void IRAM_ATTR GpioIsrHandler(void* arg) {
  BaseType_t higher_priority_woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_rx_task_handle, &higher_priority_woken);
  portYIELD_FROM_ISR(higher_priority_woken);
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
Mcp2518fd::Mcp2518fd(const PinConfig& pins, const BaudConfig& baud) : pins_(pins), baud_(baud) {}

Mcp2518fd::~Mcp2518fd() {
  if (initialized_) {
    spi_bus_remove_device(static_cast<spi_device_handle_t>(spi_device_));
    // Note: spi_bus_free() is not called here; the bus may be shared.
  }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool Mcp2518fd::Init() {
  // SPI bus config
  spi_bus_config_t bus_cfg{};
  bus_cfg.mosi_io_num = pins_.spi_mosi;
  bus_cfg.miso_io_num = pins_.spi_miso;
  bus_cfg.sclk_io_num = pins_.spi_sck;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 76;  // max CAN-FD frame in SPI object = 76 bytes

  esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE /* already init */) {
    CYMON_LOGE(kTag, "spi_bus_initialize failed: %d", ret);
    return false;
  }

  // Device config — 20 MHz SCK, CPOL=0 CPHA=0 (mode 0,0)
  spi_device_interface_config_t dev_cfg{};
  dev_cfg.clock_speed_hz = 20 * 1000 * 1000;
  dev_cfg.mode = 0;
  dev_cfg.spics_io_num = pins_.spi_cs;
  dev_cfg.queue_size = 4;
  dev_cfg.pre_cb = nullptr;
  dev_cfg.post_cb = nullptr;

  spi_device_handle_t dev;
  ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev);
  if (ret != ESP_OK) {
    CYMON_LOGE(kTag, "spi_bus_add_device failed: %d", ret);
    return false;
  }
  spi_device_ = dev;

  // Reset the MCP2518FD
  if (!SpiReset()) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(2));

  if (!ConfigureOscillator()) {
    return false;
  }
  if (!ConfigureCanFd(baud_)) {
    return false;
  }
  if (!ConfigureFifos()) {
    return false;
  }
  if (!SetOperationMode(mcp2518fd_reg::kModeNormalFd)) {
    return false;
  }

  // INT pin as input with pull-up, falling edge ISR
  gpio_config_t io_conf{};
  io_conf.pin_bit_mask = 1ULL << pins_.int_pin;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.intr_type = GPIO_INTR_NEGEDGE;
  gpio_config(&io_conf);

  gpio_install_isr_service(0);
  gpio_isr_handler_add(static_cast<gpio_num_t>(pins_.int_pin), GpioIsrHandler, nullptr);

  initialized_ = true;
  CYMON_LOGI(kTag, "MCP2518FD initialised, nominal=%u kbps, data=%u Mbps", baud_.nominal_kbps, baud_.data_mbps);
  return true;
}

// ---------------------------------------------------------------------------
// SetBaud
// ---------------------------------------------------------------------------
bool Mcp2518fd::SetBaud(const BaudConfig& baud) {
  baud_ = baud;
  if (!SetOperationMode(mcp2518fd_reg::kModeConfiguration)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(1));
  if (!ConfigureCanFd(baud)) {
    return false;
  }
  return SetOperationMode(mcp2518fd_reg::kModeNormalFd);
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------
bool Mcp2518fd::Transmit(const CanFrame& frame) {
  return WriteTxObject(frame);
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------
size_t Mcp2518fd::Receive(CanFrame* out, size_t max_frames) {
  size_t n = 0;
  while (n < max_frames) {
    // Check RX FIFO 2 status
    const uint32_t sta = SpiReadWord(mcp2518fd_reg::kCiFifoSta2);
    if ((sta & 0x01u) == 0) {
      break;  // RX FIFO empty
    }
    if (!ReadRxObject(out[n], 2)) {
      break;
    }
    out[n].timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
    ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// RxTask
// ---------------------------------------------------------------------------
void Mcp2518fd::RxTask() {
  s_rx_task_handle = xTaskGetCurrentTaskHandle();
  static CanFrame frames[8];

  for (;;) {
    // Wait for INT notification (max 10 ms to handle spurious wakeups)
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

    const size_t n = Receive(frames, 8);
    for (size_t i = 0; i < n; ++i) {
      if (rx_callback_) {
        rx_callback_(frames[i]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// SPI helpers
// ---------------------------------------------------------------------------
bool Mcp2518fd::SpiReset() {
  uint8_t cmd[2] = {0x00, 0x00};
  spi_transaction_t t{};
  t.length = 16;
  t.tx_buffer = cmd;
  return spi_device_transmit(static_cast<spi_device_handle_t>(spi_device_), &t) == ESP_OK;
}

bool Mcp2518fd::SpiWrite(uint16_t addr, const uint8_t* data, size_t len) {
  uint8_t cmd[2];
  cmd[0] = static_cast<uint8_t>((kSpiCmdWrite << 4) | ((addr >> 8) & 0x0Fu));
  cmd[1] = static_cast<uint8_t>(addr & 0xFFu);

  spi_transaction_t t{};
  // Use tx_data for small transfers or a combined buffer
  static uint8_t tx_buf[80];
  tx_buf[0] = cmd[0];
  tx_buf[1] = cmd[1];
  std::memcpy(tx_buf + 2, data, len);

  t.length = (2 + len) * 8;
  t.tx_buffer = tx_buf;
  return spi_device_transmit(static_cast<spi_device_handle_t>(spi_device_), &t) == ESP_OK;
}

bool Mcp2518fd::SpiRead(uint16_t addr, uint8_t* data, size_t len) {
  uint8_t cmd[2];
  cmd[0] = static_cast<uint8_t>((kSpiCmdRead << 4) | ((addr >> 8) & 0x0Fu));
  cmd[1] = static_cast<uint8_t>(addr & 0xFFu);

  static uint8_t tx_buf[80];
  static uint8_t rx_buf[80];
  std::memset(tx_buf, 0, 2 + len);
  tx_buf[0] = cmd[0];
  tx_buf[1] = cmd[1];

  spi_transaction_t t{};
  t.length = (2 + len) * 8;
  t.tx_buffer = tx_buf;
  t.rx_buffer = rx_buf;
  if (spi_device_transmit(static_cast<spi_device_handle_t>(spi_device_), &t) != ESP_OK) {
    return false;
  }
  std::memcpy(data, rx_buf + 2, len);
  return true;
}

bool Mcp2518fd::SpiWriteWord(uint16_t addr, uint32_t word) {
  uint8_t data[4];
  data[0] = static_cast<uint8_t>(word & 0xFFu);
  data[1] = static_cast<uint8_t>((word >> 8) & 0xFFu);
  data[2] = static_cast<uint8_t>((word >> 16) & 0xFFu);
  data[3] = static_cast<uint8_t>((word >> 24) & 0xFFu);
  return SpiWrite(addr, data, 4);
}

uint32_t Mcp2518fd::SpiReadWord(uint16_t addr) {
  uint8_t data[4]{};
  SpiRead(addr, data, 4);
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

// ---------------------------------------------------------------------------
// ConfigureOscillator
// ---------------------------------------------------------------------------
bool Mcp2518fd::ConfigureOscillator() {
  // Enable PLL (x10), SCLKDIV=1 → FSCK = 40 MHz
  // CiOSC: PLLEN=1, SCLKDIV=0 (divide by 1), CLKODIV=0
  const uint32_t osc = (1u << 0);  // PLLEN
  return SpiWriteWord(mcp2518fd_reg::kCiOsc, osc);
}

// ---------------------------------------------------------------------------
// ConfigureCanFd
// ---------------------------------------------------------------------------
bool Mcp2518fd::ConfigureCanFd(const BaudConfig& baud) {
  // Nominal bit timing
  // TQ = 1 / (BRP * Fosc)  where Fosc = 40 MHz
  // Bit time = (TSEG1 + TSEG2 + SJW) * TQ
  // For 500 kbps with 40 MHz:  BRP=1, TQ=25ns, 80 TQ/bit → 80*25ns = 2µs = 500kHz
  //   TSEG1=63, TSEG2=15, SJW=15

  const uint32_t nbrp = (40000u / baud.nominal_kbps) - 1u;  // crude, works for common rates
  const uint32_t ntseg1 = 30u;
  const uint32_t ntseg2 = 7u;
  const uint32_t nsjw = 7u;
  const uint32_t nbtcfg = (nbrp << 24) | (ntseg1 << 16) | (ntseg2 << 8) | nsjw;
  if (!SpiWriteWord(mcp2518fd_reg::kCiNbtcfg, nbtcfg)) {
    return false;
  }

  // Data bit timing for BRS
  const uint32_t dbrp = (40000u / (baud.data_mbps * 1000u)) - 1u;
  const uint32_t dtseg1 = 14u;
  const uint32_t dtseg2 = 3u;
  const uint32_t dsjw = 3u;
  const uint32_t dbtcfg = (dbrp << 24) | (dtseg1 << 16) | (dtseg2 << 8) | dsjw;
  if (!SpiWriteWord(mcp2518fd_reg::kCiDbtcfg, dbtcfg)) {
    return false;
  }

  // TDC: enable, mode=auto, offset=auto
  if (!SpiWriteWord(mcp2518fd_reg::kCiTdc, (2u << 0) | (dtseg1 << 8))) {
    return false;
  }

  // CiCON: FDF=1, ISOCRCEN=1, WAKFIL=1, DNCNT=0
  const uint32_t con = (1u << 7) |  // ISOCRCEN
                       (1u << 6) |  // WAKFIL
                       (0u << 24);  // reqop = configuration (already in config mode)
  return SpiWriteWord(mcp2518fd_reg::kCiCon, con);
}

// ---------------------------------------------------------------------------
// ConfigureFifos
// ---------------------------------------------------------------------------
bool Mcp2518fd::ConfigureFifos() {
  // TX Queue: depth=4, payload=64, priority=15
  const uint32_t txq = (3u << 0) |   // TXQSIZE=3 (4 entries)
                       (7u << 11) |  // PLSIZE=7 (64-byte payload)
                       (15u << 18);  // TXPRI=15
  if (!SpiWriteWord(mcp2518fd_reg::kCiTxQcon, txq)) {
    return false;
  }

  // FIFO1 — TX, depth=4, payload=64
  const uint32_t fifo1 = (1u << 7) |   // TXEN
                         (3u << 0) |   // FSIZE=3 (4 entries)
                         (7u << 11) |  // PLSIZE=7
                         (14u << 18);  // TXPRI=14
  if (!SpiWriteWord(mcp2518fd_reg::kCiFifocon1, fifo1)) {
    return false;
  }

  // FIFO2 — RX, depth=8, payload=64
  const uint32_t fifo2 = (0u << 7) |  // TXEN=0 (RX)
                         (7u << 0) |  // FSIZE=7 (8 entries)
                         (7u << 11);  // PLSIZE=7
  if (!SpiWriteWord(mcp2518fd_reg::kCiFifocon2, fifo2)) {
    return false;
  }

  // Accept all messages in RX FIFO2 via filter 0 → all standard+extended
  // CiFLTCON0: FLTEN0=1, F0BP=2 (FIFO2)
  if (!SpiWriteWord(0x1D0u, (1u << 7) | 2u)) {
    return false;
  }
  // CiMASK0: all zeroes → accept all
  if (!SpiWriteWord(0x1A0u, 0u)) {
    return false;
  }

  // Enable RX interrupt for FIFO2
  const uint32_t inte = (1u << 12);  // RXIE for FIFO2 (bit 12)
  return SpiWriteWord(mcp2518fd_reg::kCiIntEnable, inte);
}

// ---------------------------------------------------------------------------
// SetOperationMode
// ---------------------------------------------------------------------------
bool Mcp2518fd::SetOperationMode(uint8_t mode) {
  uint32_t con = SpiReadWord(mcp2518fd_reg::kCiCon);
  con &= ~(0x07u << 24);
  con |= (static_cast<uint32_t>(mode) << 24);
  if (!SpiWriteWord(mcp2518fd_reg::kCiCon, con)) {
    return false;
  }
  // Poll until mode is set (max 5 ms)
  for (int i = 0; i < 50; ++i) {
    vTaskDelay(1);
    const uint32_t cur = SpiReadWord(mcp2518fd_reg::kCiCon);
    if (((cur >> 21) & 0x07u) == mode) {
      return true;
    }
  }
  CYMON_LOGE(kTag, "Mode change to %u timed out", mode);
  return false;
}

// ---------------------------------------------------------------------------
// ReadRxObject
// ---------------------------------------------------------------------------
bool Mcp2518fd::ReadRxObject(CanFrame& frame, uint8_t fifo_num) {
  // Read user address from CiFIFOUA
  const uint16_t ua_reg = mcp2518fd_reg::kCiFifoua2 + static_cast<uint16_t>((fifo_num - 2) * 12);
  const uint32_t ua = SpiReadWord(ua_reg);
  const uint16_t ram_addr = static_cast<uint16_t>(0x400u + (ua & 0xFFFu));

  // MCP2518FD RX object: T0 (4), T1 (4), data (0–64), timestamp (4)
  uint8_t obj[76]{};
  if (!SpiRead(ram_addr, obj, 12)) {  // read header first
    return false;
  }

  const uint32_t t0 = static_cast<uint32_t>(obj[0]) | (static_cast<uint32_t>(obj[1]) << 8) | (static_cast<uint32_t>(obj[2]) << 16) |
                      (static_cast<uint32_t>(obj[3]) << 24);
  const uint32_t t1 = static_cast<uint32_t>(obj[4]) | (static_cast<uint32_t>(obj[5]) << 8) | (static_cast<uint32_t>(obj[6]) << 16) |
                      (static_cast<uint32_t>(obj[7]) << 24);

  frame.extended_id = (t0 >> 4) & 1u;
  frame.fd_frame = (t1 >> 4) & 1u;
  frame.brs = (t1 >> 6) & 1u;
  frame.id = (frame.extended_id) ? (t0 >> 3) & 0x1FFFFFFFu : (t0 >> 21) & 0x7FFu;

  const uint8_t dlc_raw = t1 & 0x0Fu;
  // CAN-FD DLC → byte length table
  static constexpr uint8_t kDlcTable[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
  frame.dlc = kDlcTable[dlc_raw < 16 ? dlc_raw : 0];

  if (frame.dlc > 0) {
    SpiRead(ram_addr + 8, frame.data, frame.dlc);
  }

  // Increment UINC to release the FIFO slot
  const uint16_t sta_reg = mcp2518fd_reg::kCiFifoSta2 + static_cast<uint16_t>((fifo_num - 2) * 12);
  const uint32_t con_reg = mcp2518fd_reg::kCiFifocon2 + static_cast<uint16_t>((fifo_num - 2) * 12);
  (void)sta_reg;
  uint32_t con = SpiReadWord(con_reg);
  con |= (1u << 8);  // UINC
  SpiWriteWord(con_reg, con);

  return true;
}

// ---------------------------------------------------------------------------
// WriteTxObject
// ---------------------------------------------------------------------------
bool Mcp2518fd::WriteTxObject(const CanFrame& frame) {
  // Check TX queue not full
  const uint32_t sta = SpiReadWord(mcp2518fd_reg::kCiTxQsta);
  if ((sta & 0x01u) == 0) {
    CYMON_LOGW(kTag, "TX queue full");
    return false;
  }

  const uint32_t ua = SpiReadWord(mcp2518fd_reg::kCiTxQua);
  const uint16_t ram_addr = static_cast<uint16_t>(0x400u + (ua & 0xFFFu));

  // Build T0/T1
  uint32_t t0 = 0;
  uint32_t t1 = 0;
  if (frame.extended_id) {
    t0 = ((frame.id & 0x1FFFFFFFu) << 3) | (1u << 4);  // EID + IDE
  } else {
    t0 = ((frame.id & 0x7FFu) << 21);
  }
  if (frame.fd_frame) {
    t1 |= (1u << 4);  // FDF
  }
  if (frame.brs) {
    t1 |= (1u << 6);  // BRS
  }
  // DLC — find code from length
  static constexpr uint8_t kLenTable[65] = {
      0,  1,  2,  3,  4,  5,  6,  7,  8,                               // 0–8
      9,  9,  9,  9,                                                   // 9–12 → DLC 9
      10, 10, 10, 10,                                                  // 13–16 → DLC 10
      11, 11, 11, 11,                                                  // 17–20 → DLC 11
      12, 12, 12, 12,                                                  // 21–24 → DLC 12
      13, 13, 13, 13, 13, 13, 13, 13,                                  // 25–32 → DLC 13
      14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,  // 33–48 → DLC 14
      15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15   // 49–64 → DLC 15
  };
  t1 |= (frame.dlc <= 64) ? kLenTable[frame.dlc] : 15u;

  uint8_t obj[72]{};
  obj[0] = t0 & 0xFFu;
  obj[1] = (t0 >> 8) & 0xFFu;
  obj[2] = (t0 >> 16) & 0xFFu;
  obj[3] = (t0 >> 24) & 0xFFu;
  obj[4] = t1 & 0xFFu;
  obj[5] = (t1 >> 8) & 0xFFu;
  obj[6] = (t1 >> 16) & 0xFFu;
  obj[7] = (t1 >> 24) & 0xFFu;
  std::memcpy(obj + 8, frame.data, frame.dlc);

  if (!SpiWrite(ram_addr, obj, 8 + frame.dlc)) {
    return false;
  }

  // Set TXREQ
  uint32_t con = SpiReadWord(mcp2518fd_reg::kCiTxQcon);
  con |= (1u << 9);  // TXREQ
  return SpiWriteWord(mcp2518fd_reg::kCiTxQcon, con);
}

}  // namespace cymon
