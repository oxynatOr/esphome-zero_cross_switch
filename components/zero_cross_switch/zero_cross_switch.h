#pragma once

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"

#include "driver/gptimer.h"
#include "esp_attr.h"

#include "zx_chip.h"
#include "zx_logic.h"

namespace esphome {
namespace zero_cross_switch {

// Relay switch that moves its contacts at a zero crossing of the mains voltage, like the stock Shelly firmware.
// Falls back to plain immediate switching whenever the zero-cross signal is missing or implausible.
// Some power meter ICs need a register write before their pin carries the zero crossing (e.g. the BL0942 in the Shelly
// Plug M Gen3); that is done by an optional chip object (chip_type), see bl0942.h.
class ZeroCrossSwitch : public switch_::Switch, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_relay_pin(InternalGPIOPin *pin) { relay_pin_ = pin; }
  void set_zx_pin(InternalGPIOPin *pin) { zx_pin_ = pin; }
  void set_zx_edge(gpio::InterruptType edge) { zx_edge_ = edge; }
  void set_on_time_us(uint32_t us) { on_time_us_ = us; }
  void set_off_time_us(uint32_t us) { off_time_us_ = us; }
  void set_min_frequency(float hz) { min_frequency_ = hz; }
  void set_max_frequency(float hz) { max_frequency_ = hz; }
  void set_edges_needed(uint8_t n) { edges_needed_ = n; }
  void set_edges_per_period(uint8_t n) { edges_per_period_ = n; }
  void set_max_asymmetry_us(uint32_t us) { max_asymmetry_us_ = us; }
  void set_isr_offset_us(int32_t us) { isr_offset_us_ = us; }
  void set_frequency_sensor(sensor::Sensor *s) { frequency_sensor_ = s; }
  void set_chip(ZxChip *chip) { chip_ = chip; }

 protected:
  void write_state(bool state) override;

  void apply_now_(bool state);                          // main task: switch immediately
  void IRAM_ATTR drive_relay_isr_(bool state);          // ISR: switch immediately
  static void IRAM_ATTR zx_isr_(ZeroCrossSwitch *self);  // zero-cross edge (typed argument: ESPHome's attach_interrupt template needs it)
  static bool IRAM_ATTR timer_isr_(gptimer_handle_t timer, const gptimer_alarm_event_data_t *event, void *arg);

  InternalGPIOPin *relay_pin_{nullptr};
  InternalGPIOPin *zx_pin_{nullptr};
  gpio::InterruptType zx_edge_{gpio::INTERRUPT_RISING_EDGE};
  uint32_t on_time_us_{6500};
  uint32_t off_time_us_{7800};
  float min_frequency_{45.0f};
  float max_frequency_{65.0f};
  uint8_t edges_needed_{8};
  uint8_t edges_per_period_{2};
  uint32_t max_asymmetry_us_{200};
  int32_t isr_offset_us_{0};
  sensor::Sensor *frequency_sensor_{nullptr};
  ZxChip *chip_{nullptr};

  gptimer_handle_t timer_{nullptr};
  bool timer_ok_{false};
  uint8_t relay_gpio_{0};
  bool relay_inverted_{false};

  ZxEstimator *estimator_{nullptr};
  bool was_locked_{false};  // main task: a lock existed and its loss has not been reported to the chip yet
  bool asym_reported_{false};  // main task: the asymmetry warning was logged for the current episode

  // Shared between main task and ISRs. Single writer per field; see write_state() for the ordering.
  volatile bool pending_{false};
  volatile bool request_state_{false};
  volatile bool armed_state_{false};
  volatile uint32_t request_ms_{0};
  volatile uint32_t synced_count_{0};
  volatile uint32_t unsynced_count_{0};      // written by the main task only
  volatile uint32_t isr_unsynced_count_{0};  // written by the zero-cross ISR only (no atomics needed: one writer each)
  volatile uint32_t last_delay_us_{0};

  uint32_t last_publish_ms_{0};
  uint32_t reported_synced_{0};
  uint32_t reported_unsynced_{0};
};

}  // namespace zero_cross_switch
}  // namespace esphome
