# esphome-zero_cross_switch

An ESPHome external component (platform `zero_cross_switch`) for relays that should move their contacts at a zero crossing of the mains voltage, like the stock Shelly firmware does.

It follows a zero-cross signal on a GPIO, measures the mains frequency, and delays the relay switch so the contacts move at the following zero crossing. If the signal is missing or implausible it switches immediately, like a plain `gpio` switch. Some power meter ICs need a setup write before their pin carries the zero crossing; that is done per `chip_type`.

**Status: experimental.** Tested on exactly one device (see below). Not confirmed: the contact timing itself, which only an oscilloscope can show. Read "Limits" before you switch anything that matters.

## Tested devices

| Device | `chip_type` | Result |
|---|---|---|
| Shelly Plug M Gen3 (relay GPIO0, ZX GPIO3 of the BL0942, `zx_edge: ANY`) | `bl0942` | Locks on the signal (50.00-50.02 Hz; the BL0942 reads 50.01-50.03 Hz). 6 of 6 switches synchronised, 0 immediate; delays 3505 us (on) and 2198-2208 us (off), matching the 3500 / 2200 us expected at 50 Hz. |
| other Shelly plugs / BL0942 devices | `bl0942` | untested, reports welcome |
| other power meter ICs | not supported yet | will be added after a test on real hardware |

Other devices should work if they put a clean zero-cross signal on a GPIO (`chip_type: none`), but nothing beyond the Plug M Gen3 has been verified.

## Install

```yaml
external_components:
  - source: github://oxynatOr/esphome-zero_cross_switch
    components: [zero_cross_switch]
```

For a local checkout use `type: local` with `path: components`. Examples are in `examples/`:
`zx-probe.yaml` (look at the signal first), `shelly-plug-m-gen3.yaml` (`chip_type: bl0942`), `shelly-plug-pm-gen3.yaml` (monitor only, no relay) and `generic.yaml` (`chip_type: none`).

## Usage

```yaml
switch:
  - platform: zero_cross_switch
    name: "Relay"
    relay_pin: GPIO0
    zx_pin: {number: GPIO3, mode: INPUT_PULLUP}
    zx_edge: ANY
    chip_type: bl0942      # none (default) | bl0942
    uart_id: bl0942_uart   # only with chip_type: bl0942 (required there, not allowed otherwise)
```

## How it works

The stock Shelly firmware measures the mains frequency from the zero-cross edges and drives the relay `ht - (t mod ht)` microseconds after an edge, where `ht` is the half period and `t` the mechanical relay time (6500 us on, 7800 us off). The contacts then move at the following zero crossing. This component does the same:

1. With a `chip_type`, the chip is prepared first (see below).
2. An interrupt on the zero-cross pin follows the edges (`ZxEstimator`, `zx_logic.h`).
3. A switch request waits for the next edge, arms a one-shot hardware timer (1 us resolution) with the delay, and the timer interrupt moves the relay pin directly.
4. No lock (too few clean edges, frequency outside the accepted range, stale signal, timer unavailable) or no edge within 60 ms: the relay is switched immediately. The log says so.
5. If a lock is lost (no edge for about three half periods), the chip gets a hook to repair its setup right away (`chip_type: bl0942` writes OT_FUNX again, up to 3 attempts one second apart), as the stock firmware does after two missed periods.
6. With `zx_edges_per_period: 2` the two kinds of edge interval are averaged separately. If they differ by more than `max_asymmetry_us`, the edges are not half a period apart and every second switch would miss the zero crossing; the component then does not lock and logs a hint to use one edge per period instead.

## Options

Common to all chip types:

| Option | Default | Description |
|---|---|---|
| `relay_pin` | none | Relay output. Optional: without it the component is a zero-cross monitor (see below) and the switch entity is made internal |
| `zx_pin` | required | Zero-cross input |
| `zx_edge` | required | `RISING`, `FALLING` or `ANY`: the edge(s) the component reacts to. `ANY` if the level flips at every zero crossing |
| `zx_edges_per_period` | 2 | What one edge stands for: `2` = one edge per zero crossing (a half period apart), `1` = one edge per mains period, like the stock Shelly firmware (falling edge only of a signal that toggles at every crossing). `zx_edge: ANY` requires 2 |
| `max_asymmetry_us` | 200 | With 2 edges per period: largest accepted difference between the two kinds of edge interval. `0` turns the check off |
| `chip_type` | `none` | `none` or `bl0942`, selects the chip-specific options below |
| `on_time_us` / `off_time_us` | 6500 / 7800 | Mechanical relay times. The defaults are the Shelly stock firmware numbers, not measured here; measure your own relay |
| `min_frequency` / `max_frequency` | 45 / 65 | Accepted mains frequency in Hz (stock window) |
| `edges_needed` | 8 | Clean edges in a row before the signal counts as locked |
| `isr_offset_us` | 0 | Fine correction added to the delay, -2000..2000 (needs a scope to tune). A correction that would reach the edge itself moves the switch one half period later instead |
| `frequency` | none | Optional sensor with the measured mains frequency |

### `chip_type: none`
The hardware already provides the zero-cross signal on `zx_pin`. No further options.

### `chip_type: bl0942`
GPIO3 of the Plug M Gen3 is the BL0942's multiplexed CF/ZX pin and outputs the energy pulse (CF) after a reset. About 3 s after boot the component writes register OT_FUNX = 2 over the UART (unlock first), as the stock firmware does, and repeats it every 60 s, and right away (up to 3 attempts, one second apart) when a locked signal disappears, in case the chip was reset. The pin then flips at every zero crossing (use `zx_edge: ANY`).

| Option | Description |
|---|---|
| `uart_id` | required: the UART bus of the `bl0942` sensor |

## Monitor mode (no relay)

Leave out `relay_pin` and the component only follows the zero-cross signal: it sets up the chip (`chip_type`), locks onto the edges and publishes the mains frequency (`frequency:`); the switch entity is made internal and does nothing. Use it on devices that measure but cannot switch (Shelly Plug PM Gen3: `examples/shelly-plug-pm-gen3.yaml`, not tested there yet) or to look at a signal with the real lock logic before wiring a relay. In the log you see `Zero-cross signal locked: 50.01 Hz` once the signal is accepted.

## Limits

- The relay times are Shelly's numbers. They were not measured here, and the timing cannot be proven without a scope. A wrong timing is no better than random and can be worse than switching immediately.
- Flash operations (for example saving preferences) delay the zero-cross interrupt by milliseconds. A switch that is already armed still fires on time (the timer interrupt is placed in IRAM), but a request that waits for its edge during a flash operation is mistimed.
- One relay per zero-cross pin: two `zero_cross_switch` entries on the same `zx_pin` are refused (ESP-IDF allows one interrupt handler per pin). Several relays on one signal (e.g. a power strip) are not supported yet.
- With `chip_type: bl0942`, writes share the UART with the `bl0942` sensor. A write that overlaps a poll reply can cost one sample (ESPHome drops frames with a bad checksum); it does not affect switching.
- The estimator refuses to lock on a signal with the wrong spacing, so a wrong `zx_edge` ends in plain switching, not in wrong timing.
- Do not use a `pulse_counter` on the zero-cross pin at the same time.
- ESP32 with ESP-IDF only (uses the `gptimer` driver).
- No guarantee of any kind. Mains voltage is dangerous; do not open the device while it is connected.

## Adding a chip

A chip is a small class in `components/zero_cross_switch/` (see `zx_chip.h`, `bl0942.*`; ESPHome does not copy subfolders of a component), an entry in the `chip_type` schema in `switch.py` and a branch in `to_code`. Please add one only together with a test report from real hardware.

## Checks

`tests/zx_logic_check.cpp` verifies the delay math, the offset wrap, the lock logic (including the asymmetry check and one edge per period) and the BL0942 frames at compile time (50/60 Hz, wrong signal shapes, glitches, counter wrap). `esphome config` rejects `uart_id` without `chip_type: bl0942`, `zx_edge: ANY` with `zx_edges_per_period: 1` and two switches on the same `zx_pin`. See `TESTING.md` for the tests on a device.

## License

Same as ESPHome (see `LICENSE`): the C++ files (`.h`, `.cpp`) are GPL-3.0, the Python code and everything else is MIT.
