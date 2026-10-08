#pragma once

#include "esphome/core/defines.h"

// Only built with chip_type: bl0942 (switch.py defines USE_ZERO_CROSS_BL0942 and guarantees that the uart component is loaded).
#ifdef USE_ZERO_CROSS_BL0942

#include "esphome/components/uart/uart.h"
#include "zx_chip.h"

namespace esphome {
namespace zero_cross_switch {

// The multiplexed CF/ZX pin of the BL0942 outputs the energy pulse (CF) after a reset. Register OT_FUNX = 2 turns it into a
// zero-cross output (the level flips at every zero crossing). The write goes over the UART of the bl0942 sensor: unlock first,
// then the register; repeated regularly in case the chip was reset.
class Bl0942ZxChip : public ZxChip, public uart::UARTDevice {
 public:
  void service(uint32_t ms) override;
  void on_signal_lost(uint32_t ms) override;
  void dump_config() override;

 protected:
  uint8_t phase_{0};  // 0 = idle, 1 = unlock sent, waiting to send OT_FUNX
  uint32_t sent_ms_{0};
  bool ever_sent_{false};
  bool force_{false};  // rewrite OT_FUNX as soon as possible (signal lost)
};

}  // namespace zero_cross_switch
}  // namespace esphome

#endif  // USE_ZERO_CROSS_BL0942
