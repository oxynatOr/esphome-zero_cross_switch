#pragma once
// Pure logic of the zero-cross relay timing. No ESPHome / ESP-IDF includes, so it can be checked at compile time
// (see tests/zx_logic_check.cpp).
//
// Taken from the stock Shelly Plug M Gen3 firmware (shelly_zx_synchronizer.cpp):
//   half period ht = 0.5 * 1e6 / f   [us]
//   start delay    = ht - (t mod ht) [us], t = mechanical relay time (on or off)
// The relay is driven `start delay` after a zero-cross edge, so the contacts move at the following zero crossing.

#include <cstdint>

// These helpers run inside interrupt handlers: force them inline so no flash-resident copy is ever called.
#define ZX_INLINE __attribute__((always_inline)) inline

namespace esphome {
namespace zero_cross_switch {

// Delay between a zero-cross edge and driving the relay so that the contacts move at a later zero crossing.
ZX_INLINE constexpr uint32_t start_delay_us(uint32_t relay_time_us, uint32_t half_period_us) {
  return half_period_us == 0 ? 0 : half_period_us - (relay_time_us % half_period_us);
}

// Half period in microseconds for a mains frequency given in millihertz (50000 -> 10000 us).
ZX_INLINE constexpr uint32_t half_period_us_from_mhz(uint32_t frequency_mhz) {
  return frequency_mhz == 0 ? 0 : static_cast<uint32_t>(500000000ULL / frequency_mhz);
}

// Follows the zero-cross edges. edges_per_period says what one call to on_edge() stands for:
//   2 = one edge per zero crossing (the interval between edges is a half period of the mains voltage)
//   1 = one edge per mains period (the interval is a full period, e.g. only the falling edge of a signal that toggles at every
//       zero crossing, like the stock Shelly firmware uses); the half period is then interval / 2.
// An edge interval outside the plausible window resets the lock, so a signal of the wrong kind never locks and the caller
// falls back to plain switching. max_half_period / max_age are given for the half period; they are scaled here.
class ZxEstimator {
 public:
  constexpr ZxEstimator(uint32_t min_half_period_us, uint32_t max_half_period_us, uint8_t edges_needed,
                        uint32_t max_age_us, uint8_t edges_per_period = 2)
      : min_hp_(min_half_period_us),
        max_hp_(max_half_period_us),
        need_(edges_needed),
        scale_(edges_per_period == 1 ? 2 : 1),
        max_age_(max_age_us * (edges_per_period == 1 ? 2 : 1)) {}

  ZX_INLINE constexpr void on_edge(uint32_t now_us) {
    if (have_last_) {
      const uint32_t dt = now_us - last_;  // unsigned: correct across the 32-bit wrap (every ~71 minutes)
      if (dt >= min_hp_ * scale_ && dt <= max_hp_ * scale_) {
        const uint32_t sample = dt / scale_;  // half period
        hp_ = valid_ == 0 ? sample : static_cast<uint32_t>((static_cast<uint64_t>(hp_) * 7 + sample) / 8);
        if (valid_ < 255)
          valid_++;
      } else {
        valid_ = 0;
      }
    }
    last_ = now_us;
    have_last_ = true;
  }

  // True if enough plausible edges were seen in a row and the last one is recent.
  ZX_INLINE constexpr bool locked(uint32_t now_us) const {
    return have_last_ && valid_ >= need_ && (now_us - last_) <= max_age_;
  }
  // True once edges were seen before but the last one is older than max_age (signal gone, as opposed to never seen).
  ZX_INLINE constexpr bool stale(uint32_t now_us) const { return have_last_ && (now_us - last_) > max_age_; }
  ZX_INLINE constexpr uint32_t half_period_us() const { return hp_; }
  ZX_INLINE constexpr uint8_t valid_edges() const { return valid_; }
  ZX_INLINE constexpr uint32_t last_edge_us() const { return last_; }

  // Mains frequency in millihertz (0 if unknown).
  ZX_INLINE constexpr uint32_t frequency_mhz() const { return hp_ == 0 ? 0 : static_cast<uint32_t>(500000000ULL / hp_); }

 private:
  uint32_t min_hp_, max_hp_;
  uint8_t need_;
  uint32_t scale_;
  uint32_t max_age_;
  uint32_t last_{0};
  uint32_t hp_{0};
  uint8_t valid_{0};
  bool have_last_{false};
};

}  // namespace zero_cross_switch
}  // namespace esphome
