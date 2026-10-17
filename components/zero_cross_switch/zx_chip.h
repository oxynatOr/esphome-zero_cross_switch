#pragma once

#include <cstdint>

namespace esphome {
namespace zero_cross_switch {

// Chip-specific preparation of the zero-cross signal (e.g. switching a multiplexed pin of a power meter IC to its
// zero-cross output). The switch calls setup() once and service() from its loop(); chip_type: none uses no chip object.
class ZxChip {
 public:
  virtual ~ZxChip() = default;
  virtual void setup() {}
  virtual void service(uint32_t ms) = 0;
  // Called once by the switch when a locked zero-cross signal disappeared (e.g. the chip was reset and fell back to its
  // default pin function). The chip can then repair the setup right away instead of waiting for its regular refresh.
  virtual void on_signal_lost(uint32_t ms) {}
  // Called once when the switch (re)gains a lock, e.g. to stop repair attempts.
  virtual void on_signal_restored() {}
  virtual void dump_config() {}
};

}  // namespace zero_cross_switch
}  // namespace esphome
