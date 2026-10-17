#include "zero_cross_switch.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

namespace esphome {
namespace zero_cross_switch {

static const char *const TAG = "zero_cross_switch";

// A request that no zero-cross edge picked up after this long is switched directly (the signal is probably gone).
static constexpr uint32_t REQUEST_TIMEOUT_MS = 60;
static constexpr uint32_t DIAG_INTERVAL_MS = 5000;

static inline __attribute__((always_inline)) uint32_t now_us() { return static_cast<uint32_t>(esp_timer_get_time()); }

void ZeroCrossSwitch::setup() {
  // Without a relay_pin this is a zero-cross monitor: it follows the signal (frequency, lock, chip setup) and switches nothing.
  if (this->relay_pin_ != nullptr) {
    this->relay_pin_->setup();
    this->relay_gpio_ = this->relay_pin_->get_pin();
    this->relay_inverted_ = this->relay_pin_->is_inverted();
    this->relay_pin_->digital_write(false);
  }

  // Edge intervals of a real zero-cross signal lie between the half periods of max_frequency and min_frequency.
  const uint32_t min_hp = half_period_us_from_mhz(static_cast<uint32_t>(this->max_frequency_ * 1000.0f));
  const uint32_t max_hp = half_period_us_from_mhz(static_cast<uint32_t>(this->min_frequency_ * 1000.0f));
  this->estimator_ = new ZxEstimator(min_hp, max_hp, this->edges_needed_, 3 * max_hp, this->edges_per_period_,
                                     this->max_asymmetry_us_ == 0 ? ZxEstimator::NO_ASYMMETRY_CHECK : this->max_asymmetry_us_);

  // One-shot timer with 1 us resolution that moves the relay at the right moment.
  if (this->relay_pin_ != nullptr) {
    gptimer_config_t timer_config{};
    timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_config.direction = GPTIMER_COUNT_UP;
    timer_config.resolution_hz = 1000000;
    if (gptimer_new_timer(&timer_config, &this->timer_) == ESP_OK) {
      gptimer_event_callbacks_t callbacks{};
      callbacks.on_alarm = &ZeroCrossSwitch::timer_isr_;
      if (gptimer_register_event_callbacks(this->timer_, &callbacks, this) == ESP_OK &&
          gptimer_enable(this->timer_) == ESP_OK) {
        this->timer_ok_ = true;
      }
    }
    if (!this->timer_ok_) {
      ESP_LOGW(TAG, "Hardware timer not available: switching without zero-cross synchronisation");
    }
  }

  if (this->chip_ != nullptr)
    this->chip_->setup();

  this->zx_pin_->setup();
  this->zx_pin_->attach_interrupt(&ZeroCrossSwitch::zx_isr_, this, this->zx_edge_);
}

void ZeroCrossSwitch::drive_relay_isr_(bool state) {
  gpio_ll_set_level(&GPIO, this->relay_gpio_, (state != this->relay_inverted_) ? 1 : 0);
}

void ZeroCrossSwitch::apply_now_(bool state) {
  if (this->relay_pin_ == nullptr)
    return;  // monitor mode: nothing to switch
  if (this->timer_ok_)
    gptimer_stop(this->timer_);  // drop a synchronised switch that may still be in flight
  this->relay_pin_->digital_write(state);
}

// Zero-cross edge (IRAM_ATTR is set on the declaration in the header). Every call stands for ONE zero crossing of the mains voltage.
void ZeroCrossSwitch::zx_isr_(ZeroCrossSwitch *self) {
  const uint32_t now = now_us();
  self->estimator_->on_edge(now);
  if (!self->pending_)
    return;
  self->pending_ = false;  // cleared BEFORE reading the request: a newer request sets it again
  const bool state = self->request_state_;

  const bool locked = self->timer_ok_ && self->estimator_->locked(now);
  uint32_t delay = 0;
  if (locked) {
    const uint32_t relay_time = state ? self->on_time_us_ : self->off_time_us_;
    const uint32_t hp = self->estimator_->half_period_us();
    delay = corrected_delay_us(start_delay_us(relay_time, hp), self->isr_offset_us_, hp);
  }
  if (delay == 0) {
    self->drive_relay_isr_(state);
    self->isr_unsynced_count_ = self->isr_unsynced_count_ + 1;
    return;
  }

  self->armed_state_ = state;
  self->last_delay_us_ = delay;
  gptimer_stop(self->timer_);
  gptimer_set_raw_count(self->timer_, 0);
  gptimer_alarm_config_t alarm{};
  alarm.alarm_count = delay;
  alarm.reload_count = 0;
  alarm.flags.auto_reload_on_alarm = false;
  gptimer_set_alarm_action(self->timer_, &alarm);
  gptimer_start(self->timer_);
}

bool ZeroCrossSwitch::timer_isr_(gptimer_handle_t timer, const gptimer_alarm_event_data_t *event, void *arg) {
  auto *self = static_cast<ZeroCrossSwitch *>(arg);
  self->drive_relay_isr_(self->armed_state_);
  self->synced_count_ = self->synced_count_ + 1;
  gptimer_stop(timer);
  return false;  // no higher-priority task was woken
}

void ZeroCrossSwitch::write_state(bool state) {
  if (this->relay_pin_ == nullptr) {  // monitor mode: the switch entity does nothing
    this->publish_state(state);
    return;
  }
  if (this->timer_ok_ && this->estimator_ != nullptr && this->estimator_->locked(now_us())) {
    // Synchronised: hand the request to the next zero-cross edge. Order matters: state first, then the flag.
    this->request_state_ = state;
    this->request_ms_ = millis();
    this->pending_ = true;
  } else {
    this->pending_ = false;
    this->apply_now_(state);
    this->unsynced_count_ = this->unsynced_count_ + 1;
    ESP_LOGD(TAG, "Switched immediately: no usable zero-cross signal");
  }
  this->publish_state(state);
}

void ZeroCrossSwitch::loop() {
  const uint32_t ms = millis();
  if (this->chip_ != nullptr)
    this->chip_->service(ms);
  const uint32_t t_us = now_us();
  if (this->estimator_->locked(t_us)) {
    if (!this->was_locked_) {
      ESP_LOGI(TAG, "Zero-cross signal locked: %.2f Hz, edge asymmetry %u us",
               (double) (this->estimator_->frequency_mhz() / 1000.0f), (unsigned) this->estimator_->asymmetry_us());
      if (this->chip_ != nullptr)
        this->chip_->on_signal_restored();
    }
    this->was_locked_ = true;
    this->asym_reported_ = false;
  } else if (this->estimator_->stale(t_us)) {
    if (this->was_locked_) {
      this->was_locked_ = false;  // report a lost signal once per lock
      ESP_LOGW(TAG, "Zero-cross signal lost");
      if (this->chip_ != nullptr)
        this->chip_->on_signal_lost(ms);
    }
  } else if (this->estimator_->asymmetric() && !this->asym_reported_) {
    ESP_LOGW(TAG,
             "Zero-cross edges are not half a period apart (asymmetry %u us > %u us): not locking. Use zx_edge FALLING "
             "(or RISING) with zx_edges_per_period: 1",
             (unsigned) this->estimator_->asymmetry_us(), (unsigned) this->max_asymmetry_us_);
    this->asym_reported_ = true;
  }
  if (this->pending_ && (ms - this->request_ms_) > REQUEST_TIMEOUT_MS) {
    this->pending_ = false;
    this->apply_now_(this->request_state_);
    this->unsynced_count_ = this->unsynced_count_ + 1;
    ESP_LOGW(TAG, "No zero-cross edge within %u ms: switched immediately", (unsigned) REQUEST_TIMEOUT_MS);
  }

  if (ms - this->last_publish_ms_ < DIAG_INTERVAL_MS)
    return;
  this->last_publish_ms_ = ms;
  if (this->frequency_sensor_ != nullptr && this->estimator_->locked(now_us())) {
    this->frequency_sensor_->publish_state(this->estimator_->frequency_mhz() / 1000.0f);
  }
  const uint32_t synced = this->synced_count_;
  const uint32_t unsynced = this->unsynced_count_ + this->isr_unsynced_count_;
  if (synced != this->reported_synced_ || unsynced != this->reported_unsynced_) {
    ESP_LOGI(TAG, "Switches so far: %u synchronised, %u immediate; last delay %u us; zero-cross %s", (unsigned) synced,
             (unsigned) unsynced, (unsigned) this->last_delay_us_,
             this->estimator_->locked(now_us()) ? "locked" : "NOT locked");
    this->reported_synced_ = synced;
    this->reported_unsynced_ = unsynced;
  }
}

void ZeroCrossSwitch::dump_config() {
  LOG_SWITCH("", "Zero-cross relay", this);
  if (this->relay_pin_ != nullptr) {
    LOG_PIN("  Relay pin: ", this->relay_pin_);
  } else {
    ESP_LOGCONFIG(TAG, "  No relay pin: zero-cross monitor only, nothing is switched");
  }
  LOG_PIN("  Zero-cross pin: ", this->zx_pin_);
  if (this->relay_pin_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Relay time: on %u us, off %u us, offset %d us", (unsigned) this->on_time_us_,
                  (unsigned) this->off_time_us_, (int) this->isr_offset_us_);
  }
  ESP_LOGCONFIG(TAG, "  Accepted mains frequency: %.1f - %.1f Hz, %u clean edges to lock, %u edge(s) per mains period",
                (double) this->min_frequency_, (double) this->max_frequency_, (unsigned) this->edges_needed_,
                (unsigned) this->edges_per_period_);
  if (this->edges_per_period_ == 2) {
    if (this->max_asymmetry_us_ == 0) {
      ESP_LOGCONFIG(TAG, "  Edge asymmetry check: off");
    } else {
      ESP_LOGCONFIG(TAG, "  Edge asymmetry check: max %u us", (unsigned) this->max_asymmetry_us_);
    }
  }
  if (this->chip_ != nullptr) {
    this->chip_->dump_config();
  } else {
    ESP_LOGCONFIG(TAG, "  Chip: none (zero-cross signal is provided by the hardware)");
  }
  if (this->relay_pin_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Hardware timer: %s", this->timer_ok_ ? "ok" : "NOT available (immediate switching)");
  LOG_SENSOR("  ", "Frequency", this->frequency_sensor_);
}

}  // namespace zero_cross_switch
}  // namespace esphome
