// Compile-time checks of zx_logic.h and bl0942_frames.h. If this file compiles, the checks passed:
//   riscv32-esp-elf-g++ -std=gnu++17 -fsyntax-only -I ../components/zero_cross_switch zx_logic_check.cpp
#include "zx_logic.h"
#include "bl0942_frames.h"

using namespace esphome::zero_cross_switch;

// --- start delay: the numbers the stock firmware computes (on = 6500 us, off = 7800 us) ---
static_assert(start_delay_us(6500, 10000) == 3500, "50 Hz turn-on");
static_assert(start_delay_us(7800, 10000) == 2200, "50 Hz turn-off");
static_assert(start_delay_us(6500, 8333) == 1833, "60 Hz turn-on");
static_assert(start_delay_us(7800, 8333) == 533, "60 Hz turn-off");
static_assert(start_delay_us(12500, 10000) == 7500, "relay time longer than a half period wraps");
static_assert(start_delay_us(10000, 10000) == 10000, "exact multiple waits one full half period");
static_assert(start_delay_us(0, 10000) == 10000, "zero time");
static_assert(start_delay_us(6500, 0) == 0, "unknown half period");
// contacts move at (delay + relay time), which must be a multiple of the half period
static_assert((start_delay_us(6500, 10000) + 6500) % 10000 == 0, "lands on a zero crossing (on)");
static_assert((start_delay_us(7800, 10000) + 7800) % 10000 == 0, "lands on a zero crossing (off)");
static_assert((start_delay_us(6500, 8333) + 6500) % 8333 == 0, "lands on a zero crossing, 60 Hz");

// --- half period from frequency ---
static_assert(half_period_us_from_mhz(50000) == 10000, "50 Hz");
static_assert(half_period_us_from_mhz(60000) == 8333, "60 Hz");
static_assert(half_period_us_from_mhz(0) == 0, "no frequency");

// --- estimator ---
constexpr uint32_t kMin = 7692;    // 65 Hz
constexpr uint32_t kMax = 11111;   // 45 Hz
constexpr uint32_t kAge = 30000;

// locks after 8 plausible intervals (9 edges) at 50 Hz and reports the right half period
constexpr ZxEstimator feed50(int edges, uint32_t start = 0) {
  ZxEstimator e(kMin, kMax, 8, kAge);
  uint32_t t = start;
  for (int i = 0; i < edges; i++) {
    e.on_edge(t);
    t += 10000;
  }
  return e;
}
static_assert(!feed50(8).locked(70000), "8 edges are only 7 intervals");
static_assert(feed50(9).locked(80000), "9 edges are 8 intervals");
static_assert(feed50(9).half_period_us() == 10000, "half period");
static_assert(feed50(9).frequency_mhz() == 50000, "frequency 50.000 Hz");

// the lock expires when the edges stop
static_assert(feed50(20).locked(190000 + 25000), "recent edge");
static_assert(!feed50(20).locked(190000 + 31000), "stale edge");

// a signal with one pulse per cycle (20 ms) never locks -> caller falls back to plain switching
constexpr ZxEstimator feed_per_cycle() {
  ZxEstimator e(kMin, kMax, 8, kAge);
  for (uint32_t i = 0; i < 30; i++)
    e.on_edge(i * 20000);
  return e;
}
static_assert(!feed_per_cycle().locked(29 * 20000), "20 ms spacing is not a zero-cross signal");

// a signal with both flanks of a short pulse (e.g. 1 ms, 9 ms) never locks either
constexpr ZxEstimator feed_both_flanks() {
  ZxEstimator e(kMin, kMax, 8, kAge);
  uint32_t t = 0;
  for (int i = 0; i < 30; i++) {
    e.on_edge(t);
    t += 1000;
    e.on_edge(t);
    t += 9000;
  }
  return e;
}
static_assert(!feed_both_flanks().locked(300000), "pulse flanks are not a zero-cross signal");

// one glitch resets the lock; it is regained after enough good edges
constexpr ZxEstimator feed_glitch() {
  ZxEstimator e = feed50(20);
  e.on_edge(190000 + 3000);  // spurious edge 3 ms after the last one
  return e;
}
static_assert(!feed_glitch().locked(193000), "glitch resets the lock");

// works across the 32-bit microsecond wrap (about every 71.6 minutes)
static_assert(feed50(20, 0xFFFFFFFFu - 50000).locked((0xFFFFFFFFu - 50000) + 19 * 10000 + 5000),
              "lock survives the counter wrap");
static_assert(feed50(20, 0xFFFFFFFFu - 50000).half_period_us() == 10000, "half period across the wrap");

// 60 Hz is accepted too
constexpr ZxEstimator feed60() {
  ZxEstimator e(kMin, kMax, 8, kAge);
  for (uint32_t i = 0; i < 20; i++)
    e.on_edge(i * 8333);
  return e;
}
static_assert(feed60().locked(19 * 8333), "60 Hz locks");
static_assert(feed60().frequency_mhz() == 60002 || feed60().frequency_mhz() == 60000, "approx. 60 Hz");

// --- one edge per mains period (stock Shelly: falling edge only): interval is a full period, half period = interval / 2 ---
constexpr ZxEstimator feed_period(int edges, uint32_t interval, uint32_t start = 0) {
  ZxEstimator e(kMin, kMax, 8, kAge, 1);
  uint32_t t = start;
  for (int i = 0; i < edges; i++) {
    e.on_edge(t);
    t += interval;
  }
  return e;
}
static_assert(feed_period(20, 20000).locked(19 * 20000), "50 Hz, one edge per period locks");
static_assert(feed_period(20, 20000).half_period_us() == 10000, "half period from the full period");
static_assert(feed_period(20, 20000).frequency_mhz() == 50000, "frequency from the full period");
static_assert(!feed_period(20, 10000).locked(19 * 10000), "half-period spacing is wrong for edges_per_period = 1");
static_assert(!feed_period(8, 20000).locked(7 * 20000), "needs enough edges");
static_assert(feed_period(20, 20000).locked(19 * 20000 + 55000), "lock lasts longer with 1 edge per period");
static_assert(!feed_period(20, 20000).locked(19 * 20000 + 61000), "but expires");
static_assert(!feed50(20).stale(190000 + 25000), "recent edge is not stale");
static_assert(feed50(20).stale(190000 + 31000), "old edge is stale");
static_assert(!ZxEstimator(kMin, kMax, 8, kAge).stale(1000000), "never seen an edge: not stale (never locked)");
// 2 edges per period (default) still rejects a 20 ms spacing
static_assert(!feed_per_cycle().locked(29 * 20000), "default mode rejects one edge per period");

// --- fine correction (isr_offset_us): never moves the drive time to the edge itself, wraps by half periods instead ---
static_assert(corrected_delay_us(3500, 0, 10000) == 3500, "no offset");
static_assert(corrected_delay_us(3500, 200, 10000) == 3700, "positive offset");
static_assert(corrected_delay_us(2200, -500, 10000) == 1700, "small negative offset");
static_assert(corrected_delay_us(2200, -2000, 10000) == 200, "negative offset close to the edge");
static_assert(corrected_delay_us(2200, -2200, 10000) == 10000, "offset onto the edge wraps one half period");
static_assert(corrected_delay_us(1000, -2000, 10000) == 9000, "offset past the edge wraps one half period");
static_assert(corrected_delay_us(533, -2000, 8333) == 6866, "60 Hz wrap");
static_assert(corrected_delay_us(3500, -100, 0) == 0, "unknown half period: switch now");

// --- edge asymmetry (2 edges per period): intervals alternate a / b ---
constexpr ZxEstimator feed_asym(uint32_t a, uint32_t b, uint32_t max_asym, int pairs = 15) {
  ZxEstimator e(kMin, kMax, 8, kAge, 2, max_asym);
  uint32_t t = 0;
  for (int i = 0; i < pairs; i++) {
    e.on_edge(t);
    t += a;
    e.on_edge(t);
    t += b;
  }
  return e;
}
constexpr uint32_t kEndAsym(uint32_t a, uint32_t b, int pairs = 15) { return (a + b) * (pairs - 1) + a; }
static_assert(feed_asym(10000, 10000, 200).locked(kEndAsym(10000, 10000)), "symmetric signal locks");
static_assert(feed_asym(10000, 10000, 200).asymmetry_us() == 0, "no asymmetry");
static_assert(feed_asym(10050, 9950, 200).locked(kEndAsym(10050, 9950)), "100 us asymmetry is accepted");
static_assert(feed_asym(10050, 9950, 200).half_period_us() >= 9990 && feed_asym(10050, 9950, 200).half_period_us() <= 10010,
              "half period is the mean");
static_assert(!feed_asym(9000, 11000, 200).locked(kEndAsym(9000, 11000)), "45/55 duty cycle does not lock");
static_assert(feed_asym(9000, 11000, 200).asymmetric(), "and is reported as asymmetric");
static_assert(feed_asym(9000, 11000, 200).asymmetry_us() > 1500, "asymmetry about 2000 us");
static_assert(feed_asym(9000, 11000, ZxEstimator::NO_ASYMMETRY_CHECK).locked(kEndAsym(9000, 11000)),
              "check disabled: locks (old behaviour)");
static_assert(!feed50(20).asymmetric(), "default estimator has no asymmetry check");
static_assert(feed_period(20, 20000).asymmetry_us() == 0 && !feed_period(20, 20000).asymmetric(),
              "1 edge per period: every interval alike, no check");

// --- BL0942 write frames: the bytes the hardware test sent (and the stock firmware builds) ---
constexpr bool same(Bl0942Frame f, const uint8_t (&e)[6]) {
  for (int i = 0; i < 6; i++)
    if (f.bytes[i] != e[i])
      return false;
  return true;
}
static_assert(same(bl0942_write_frame(BL0942_REG_USR_WRPROT, BL0942_UNLOCK_VALUE), {0xA8, 0x1D, 0x55, 0x00, 0x00, 0xE5}),
              "unlock frame");
static_assert(same(bl0942_write_frame(BL0942_REG_OT_FUNX, BL0942_OT_FUNX_ZX), {0xA8, 0x18, 0x02, 0x00, 0x00, 0x3D}),
              "OT_FUNX = 2 frame");
static_assert(same(bl0942_write_frame(BL0942_REG_OT_FUNX, 0), {0xA8, 0x18, 0x00, 0x00, 0x00, 0x3F}),
              "OT_FUNX = 0 frame");

int main() { return 0; }
