#include "bl0942.h"

#ifdef USE_ZERO_CROSS_BL0942

#include "bl0942_frames.h"
#include "esphome/core/log.h"

namespace esphome {
namespace zero_cross_switch {

static const char *const TAG = "zero_cross_switch.bl0942";

// The bl0942 sensor resets the chip and writes its MODE register during its own setup; the pin function is written after that
// and refreshed regularly in case the chip was reset (it falls back to the CF pulse output after a reset).
static constexpr uint32_t FIRST_WRITE_MS = 3000;
static constexpr uint32_t REFRESH_MS = 60000;
static constexpr uint32_t GAP_MS = 50;  // pause between the unlock and the register write
// The stock firmware re-runs the chip setup after two mains periods without an edge, up to three times. Do the same, one
// attempt per second; after that only the regular refresh is left.
static constexpr uint32_t REPAIR_INTERVAL_MS = 1000;
static constexpr uint8_t REPAIR_ATTEMPTS = 3;

void Bl0942ZxChip::service(uint32_t ms) {
  if (this->phase_ == 0) {
    const bool regular = this->ever_sent_ ? (ms - this->sent_ms_) >= REFRESH_MS : ms >= FIRST_WRITE_MS;
    const bool forced = this->repairs_left_ > 0 && (!this->ever_sent_ || (ms - this->sent_ms_) >= REPAIR_INTERVAL_MS);
    if (!regular && !forced)
      return;
    if (forced) {
      ESP_LOGW(TAG, "Zero-cross signal lost: writing OT_FUNX = 2 again (attempt %u of %u)",
               (unsigned) (REPAIR_ATTEMPTS - this->repairs_left_ + 1), (unsigned) REPAIR_ATTEMPTS);
      this->repairs_left_--;
    }
    const Bl0942Frame unlock = bl0942_write_frame(BL0942_REG_USR_WRPROT, BL0942_UNLOCK_VALUE);
    this->write_array(unlock.bytes, sizeof(unlock.bytes));
    this->sent_ms_ = ms;
    this->phase_ = 1;
  } else if ((ms - this->sent_ms_) >= GAP_MS) {
    const Bl0942Frame function = bl0942_write_frame(BL0942_REG_OT_FUNX, BL0942_OT_FUNX_ZX);
    this->write_array(function.bytes, sizeof(function.bytes));
    this->sent_ms_ = ms;
    this->phase_ = 0;
    if (!this->ever_sent_)
      ESP_LOGI(TAG, "BL0942 pin set to zero-cross output (OT_FUNX = 2)");
    this->ever_sent_ = true;
  }
}

void Bl0942ZxChip::on_signal_lost(uint32_t ms) {
  // The first attempt starts right away unless a write happened less than REPAIR_INTERVAL_MS ago.
  this->repairs_left_ = REPAIR_ATTEMPTS;
}

void Bl0942ZxChip::on_signal_restored() {
  if (this->repairs_left_ != REPAIR_ATTEMPTS && this->repairs_left_ != 0)
    ESP_LOGI(TAG, "Zero-cross signal back after %u repair attempt(s)", (unsigned) (REPAIR_ATTEMPTS - this->repairs_left_));
  this->repairs_left_ = 0;
}

void Bl0942ZxChip::dump_config() { ESP_LOGCONFIG(TAG, "  Chip: BL0942, pin to zero-cross output (OT_FUNX = 2)"); }

}  // namespace zero_cross_switch
}  // namespace esphome

#endif  // USE_ZERO_CROSS_BL0942
