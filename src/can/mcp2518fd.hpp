#pragma once

#include <cstdint>
#include <functional>

#include "transport/can_frame.hpp"

namespace cymon {

/// Callback invoked from the RX task for every received CAN frame.
using CanRxCallback = std::function<void(const CanFrame&)>;

/// MCP2518FD CAN-FD controller driver.
///
/// Uses ESP-IDF spi_master_driver with SPI-DMA.  A GPIO INT pin drives a
/// FreeRTOS direct task notification so the RX task is woken immediately on
/// frame arrival.
///
/// All public methods (except the ISR hook) must be called from the same task
/// or with external locking.
class Mcp2518fd {
 public:
  /// Hardware pin configuration.
  struct PinConfig {
    int spi_mosi{11};
    int spi_miso{13};
    int spi_sck{12};
    int spi_cs{10};
    int int_pin{9};    ///< active-low interrupt
    int stby_pin{-1};  ///< optional standby pin (-1 = not connected)
  };

  struct BaudConfig {
    uint32_t nominal_kbps{500};
    uint32_t data_mbps{2};
    uint32_t osc_hz{40000000};  ///< MCP2518FD external oscillator / crystal Hz
  };

  explicit Mcp2518fd(const PinConfig& pins, const BaudConfig& baud);
  ~Mcp2518fd();

  // Non-copyable, non-movable
  Mcp2518fd(const Mcp2518fd&) = delete;
  Mcp2518fd& operator=(const Mcp2518fd&) = delete;

  /// Initialise SPI bus, configure the chip, install INT ISR.
  /// Must be called before any other method.
  bool Init();

  /// Reconfigure baud rates (drains TX, resets chip, reconfigures).
  bool SetBaud(const BaudConfig& baud);

  /// Transmit a CAN-FD frame.  Blocks until space is available in the TX FIFO.
  /// Returns false if the chip is not responsive.
  bool Transmit(const CanFrame& frame);

  /// Receive up to @p max_frames frames from the RX FIFO into @p out.
  /// Returns the number of frames written.
  size_t Receive(CanFrame* out, size_t max_frames);

  /// Register a callback invoked from the RX task for each received frame.
  void SetRxCallback(CanRxCallback cb) {
    rx_callback_ = std::move(cb);
  }

  /// FreeRTOS task body — call from a pinned high-priority task on core 1.
  void RxTask();

 private:
  // SPI helpers
  bool SpiWrite(uint16_t addr, const uint8_t* data, size_t len);
  bool SpiRead(uint16_t addr, uint8_t* data, size_t len);
  bool SpiWriteWord(uint16_t addr, uint32_t word);
  [[nodiscard]] uint32_t SpiReadWord(uint16_t addr);
  bool SpiReset();

  // MCP2518FD configuration
  bool ConfigureOscillator();
  bool ConfigureCanFd(const BaudConfig& baud);
  bool ConfigureFifos();
  bool SetOperationMode(uint8_t mode);

  // Frame I/O
  bool ReadRxObject(CanFrame& frame, uint8_t fifo_num);
  bool WriteTxObject(const CanFrame& frame);

  PinConfig pins_;
  BaudConfig baud_;

  void* spi_device_{nullptr};  ///< spi_device_handle_t (opaque)
  CanRxCallback rx_callback_;
  bool initialized_{false};
};

// MCP2518FD SFR addresses
namespace mcp2518fd_reg {
inline constexpr uint16_t kCiCon = 0x000;
inline constexpr uint16_t kCiNbtcfg = 0x004;
inline constexpr uint16_t kCiDbtcfg = 0x008;
inline constexpr uint16_t kCiTdc = 0x00C;
inline constexpr uint16_t kCiTscon = 0x014;
inline constexpr uint16_t kCiVec = 0x018;
inline constexpr uint16_t kCiInt = 0x01C;
inline constexpr uint16_t kCiIntFlag = 0x01C;
inline constexpr uint16_t kCiIntEnable = 0x020;
inline constexpr uint16_t kCiRxovif = 0x028;
inline constexpr uint16_t kCiTxatif = 0x02C;
inline constexpr uint16_t kCiTxreq = 0x030;
inline constexpr uint16_t kCiTxQcon = 0x050;
inline constexpr uint16_t kCiTxQsta = 0x054;
inline constexpr uint16_t kCiTxQua = 0x058;
inline constexpr uint16_t kCiFifocon1 = 0x05C;
inline constexpr uint16_t kCiFifoSta1 = 0x060;
inline constexpr uint16_t kCiFifoua1 = 0x064;
inline constexpr uint16_t kCiFifocon2 = 0x068;
inline constexpr uint16_t kCiFifoSta2 = 0x06C;
inline constexpr uint16_t kCiFifoua2 = 0x070;
inline constexpr uint16_t kCiOsc = 0xE00;
inline constexpr uint16_t kCiIocon = 0xE04;

// CiCON mode bits
inline constexpr uint8_t kModeNormalFd = 0x00;
inline constexpr uint8_t kModeConfiguration = 0x04;
inline constexpr uint8_t kModeSleep = 0x01;
inline constexpr uint8_t kModeListenOnly = 0x03;
}  // namespace mcp2518fd_reg

}  // namespace cymon
