#pragma once
// BL0942 register write frames. Pure logic, no ESPHome includes, so it can be checked at compile time.
//
// Frame: 0xA8, register, 3 data bytes (little endian), checksum = ~(sum of the first five bytes). Same format the stock
// Shelly firmware (shos_bl0942_uart.c) and the ESPHome bl0942 sensor use.

#include <cstdint>

namespace esphome {
namespace zero_cross_switch {

static constexpr uint8_t BL0942_WRITE_HEADER = 0xA8;
static constexpr uint8_t BL0942_REG_OT_FUNX = 0x18;     // output pin function: 0 = CF energy pulse, 2 = zero crossing
static constexpr uint8_t BL0942_REG_USR_WRPROT = 0x1D;  // write 0x55 to unlock the writable registers
static constexpr uint8_t BL0942_UNLOCK_VALUE = 0x55;
static constexpr uint8_t BL0942_OT_FUNX_ZX = 2;

struct Bl0942Frame {
  uint8_t bytes[6];
};

constexpr Bl0942Frame bl0942_write_frame(uint8_t reg, uint32_t value) {
  const uint8_t d0 = static_cast<uint8_t>(value & 0xFF);
  const uint8_t d1 = static_cast<uint8_t>((value >> 8) & 0xFF);
  const uint8_t d2 = static_cast<uint8_t>((value >> 16) & 0xFF);
  const uint8_t sum = static_cast<uint8_t>(BL0942_WRITE_HEADER + reg + d0 + d1 + d2);
  return Bl0942Frame{{BL0942_WRITE_HEADER, reg, d0, d1, d2, static_cast<uint8_t>(~sum)}};
}

}  // namespace zero_cross_switch
}  // namespace esphome
