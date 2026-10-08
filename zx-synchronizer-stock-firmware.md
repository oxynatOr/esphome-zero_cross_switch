# Stock Shelly zero-cross switching ("ZX synchronizer") - reverse engineering notes

Source: static analysis (strings + RISC-V disassembly with capstone) of the flash dumps in `_Input_/Shelly_Dumps`.
Nothing here was measured on a device except where marked **(measured)**. Everything marked **(inferred)** is a reading of
disassembly, not a confirmed behaviour. Tools to reproduce: `__Idea/tools/` (see the end).

## 1. Where the logic lives

The stock firmware has a library `shelly-zx-synchronizer` (`/APP/libs/shelly-zx-synchronizer/src/shelly_zx_synchronizer.cpp`).
The word "cross switch" does not appear anywhere; the strings are `ZX synchronizer ...`, `shelly::ZXSynchronizer`,
`ZXSyncNativeGpio`, `ZXSyncNonNativeGpio`, `ZXCapablePowerMeterChip`.

| Dump | Firmware | ZX library |
|---|---|---|
| `shelly_plug_pm_g3_3.bin` | 1.8.99-plugmg3prod0 (ESP32-C3), slot **app_1** @0x3a0000 | yes (app_0 @0x20000 is older: BL0942 ZX IRQ only, no synchronizer) |
| `powerstrip4_gen4_backup_1.bin`, `dump_2.bin` | 1.7.99-powerstripg4prod1 (ESP32-C6), both slots | yes (newer code, same algorithm) |
| `shelly_1pm_mini_g4_1`, `shelly_plug_gen3` (1.2.3), `sheely_plugs_gen3_nr4`, `pm1_minig3`, bulbs, Cury, HT3, Pill | - | **no** ZX strings |

(The `crosscore_int` / "Cross-device link" / "interlocks" hits are ESP-IDF and Matter, unrelated.)

Image layout of the Plug PM G3 dump: partition table at 0x10000, `app_0` 0x20000, `app_1` 0x3a0000 (size 0x2a0000 each),
`shelly` 0x7f0000 (calibration + matter data only, **no** ZX/relay parameters in it). Virtual address of a file offset in an
app image: see `__Idea/tools/esp_image.py` (e.g. app_1 rodata `0x3c230020`, text `0x42000020`, IRAM `0x40380000`).

## 2. Architecture (Plug PM G3, app_1 virtual addresses)

```
BL0942 ZX pin --falling edge--> ZXVIRQHandler (IRAM 0x4038237a, registered in BL0942PowerMeter::Init, pin pulled UP)
      |  stores esp_timer timestamp of the edge, clears the IRQ-timeout retry counter
      v
ZXCapablePowerMeterChip callback slot (chip+0x30, i.e. pm+0x88) -> ZXSyncNativeGpio::OnZX (IRAM 0x40382476/0x40382424)
      v
two chained one-shot hw timers (shos_hw_timers.c) -> Apply(state) -> per-request callback -> output peripheral drives the relay GPIO
```

- Instance creation: `shelly_init.cpp`, call at `0x42007a56`. Arguments (decoded from the instruction immediates):
  `ZXSyncNativeGpio(this, id=0, chip=&pm_chip, on_reaction_time_us=6500, off_reaction_time_us=7800, capacity=3)`.
  `capacity` goes to `std::vector::reserve` for the request list (0x28-byte entries), so it is the number of outputs/pending requests.
  Only one `ZXSyncNativeGpio` may exist (`s_instance_` assert).
- The Power Strip Gen4 (ESP32-C6) calls its constructor (`0x4200c6ce`, app_0) with the **same values 6500 / 7800 / 3**. So the
  mechanical relay times are not per-device constants of the Plug M but shared defaults of the current Shelly relay.
  The Gen4 constructor only asserts `>= 0` (Plug asserts `> 0`), and `on != off` in both.
- `ZXSynchronizer::Init` (0x4215b3d6) runs once, reads the chip's frequency (`chip+0x0c`, float, from the BL0942 FREQ register that
  the PM code polls; "Failed to read Frequency" is its error), and
  1. rejects NaN, `f < 45.0` or `f > 65.0` (constants in rodata) -> `ZX synchronizer frequency check failed! (%fHz)`, `Bad AC frequency %.2f`
  2. `ht = (int)(1.0 / f * 1e6 * 0.5)` (half period in us; double precision, truncating)
  3. `on_delay = delay(on_reaction_time, ht)`, `off_delay = delay(off_reaction_time, ht)` with
     `delay(t, ht) = (t < 0 || ht <= 0) ? 0 : ht - (t % ht)` (function at 0x4215b3c2)
  4. logs `ZX times ht=%d on=%dus (delay %dus) off=%dus (delay %dus)` once. At 50 Hz: ht 10000, on delay 3500, off delay 2200
     **(matches the 3505 / 2198-2208 us measured with the component)**.
  `ht` and both delays are **computed once and never updated**: no re-measurement while running.

### 2.1 What happens at a zero-cross edge (`OnZX`, IRAM)

```
OnZX():
  if (!init_ok) return;
  if (timer_handle != 0) return;                 // a chain from the previous edge is still running -> ignore this edge
  if (off_delay >= on_delay) timer_handle = hw_timer(on_delay,  timerA)   // earlier delay first
  else                       timer_handle = hw_timer(off_delay, timerB)
  if (timer_handle == 0) -> "Acquiring hw timer failed!" -> Fallback()

timerA():  free timer; if (on_delay < off_delay) timer_handle = hw_timer(off_delay - on_delay, timerB);  Apply(state=1)
timerB():  free timer; if (off_delay < on_delay) timer_handle = hw_timer(on_delay - off_delay, timerA);  Apply(state=0)

Apply(s):  for each pending request r with r.state == s and not done: r.callback(r.arg); r.done = 1
```

So both directions are served inside the same half period; the smaller delay fires first, the second timer is armed relative to the
first (relative timing, not two absolute delays). Hw timer start is `shos_hw_timer_start(delay_us, flags=0x10000, cb, arg)`
(0x40382638 region, table of 0x1c-byte slots; "No HW timers available." when exhausted).

### 2.2 Request path (what a Switch.Set does)

`shelly_output_peripheral.cpp` (`0x4209ce1a`): if the peripheral was created with a synchronizer (always, "Invalid zx_synchronizer"
assert otherwise) it calls `Request(output_id, state, callback = direct_set_trampoline @0x4038236e, arg = peripheral)`.
- `Request` (0x4215b990): returns false if `init_ok == 0` or id < 0 or callback null; otherwise it **replaces** an existing pending
  request of the same output id (so only the newest state survives) or appends one, then returns true.
- If it returns false the peripheral logs `ZX synchronizer request failed: fallback to unsync-ed switching` and switches directly
  (the callback is the very same "set output now" virtual call).
- The request is not executed by Request itself. It waits for the next ZX edge. **Latency is up to one mains period (20 ms at 50 Hz)
  plus the delay.** There is no per-request timeout in the synchronizer; the protection is the IRQ watchdog below.

### 2.3 Failure handling

- **IRQ watchdog (BL0942PowerMeter, `0x420c8d3e`)**: periodic timer (period `1.5 * timeout`); timeout =
  `2 * (int)(1e6 / roundf(f)) / 1000` ms, i.e. **two mains periods (40 ms at 50 Hz)** without a ZX edge. It then logs
  `pm chip id %d IRQ timeout (mode %d), retrying setup (attempt %d)...` and re-runs the chip setup (re-writes OT_FUNX). After more than
  2 retries it notifies the listener (`0x4215b084`, event 1): `ZXV IRQ timeout!` -> `Fallback()`.
- **Fallback()** (0x4215b72a): flushes every pending request by calling its callback directly (the synchronizer itself is not
  disabled by this call, **inferred**: `init_ok` is not cleared in that function).
- **OTA**: event `0x4f544103` (OTA start) disables the synchronizer (`init_ok = 0`, timer cancelled, requests flushed,
  `OTA in progress: synchronizer disabled`), `0x4f544107` re-enables it.
- `ZX synchronizer hw timer setup wd triggered` (0x4215bbb0, **inferred**): a callback registered in `NativeInit` that fires if the
  hw-timer setup was not finished; it cancels the timer handle. `Init` also starts a 50 ms (0xc350 us) timer with callback 0x4215b22c; purpose not decoded.

## 3. BL0942 side (stock `shos_bl0942.c`, `shelly_pm_bl0942.cpp`)

- The IRQ pin is `irq_pin_`, set up as input with **pull-up**, interrupt type value `2` (the assertion text says
  `SHOS_GPIO_INT_EDGE_NEG`): **only the falling edge** triggers. (Component notes say the pin toggles at every zero crossing,
  ~6000 edges/min = both flanks -> the stock therefore syncs on **one zero-crossing polarity, once per mains period**, not on every crossing.)
- The chip is switched to ZX output by the synchronizer's init-done listener (`0x4215b0c2`): once the PM chip finished init and
  is ZX-capable it calls the chip's `SetZXMode(2)` (vtable +8); on OTA/shutdown it calls `SetZXMode(1)` (vtable +8) / `-1` (vtable +0xc).
- `SetZXMode(2)` (0x420c8c0c) stores mode 2, calls `SetFunx(chip, 2)` and arms the watchdog; modes 0/1 call `SetFunx(chip, 0)` (back to CF pulse).
- `SetFunx(chip, v)` (0x420c628a): two BL0942 UART register writes, in this order
  1. `USR_WRPROT (0x1D) = 0x55` (3 data bytes `55 00 00`)
  2. `OT_FUNX (0x18) = v` (3 data bytes `v 00 00`)
  The frame format (`A8 reg d0 d1 d2 chk`, `chk = ~sum`) is what the component already uses; it was not re-derived from the stock
  disassembly (the checksum helper at 0x420c6354 starts with 0x58, the *read* header, i.e. it is used for the read path).
  After a UART packet-read timeout (`%p: packet read timed out`, shos_bl0942.c) the chip state is reset and the stored funx value is **re-sent**
  - same idea as the component's 60 s refresh.
- Energy/power are read over UART registers, **not** from the CF pulse, so using the pin for ZX costs nothing.

## 4. Differences: stock vs. the existing `shelly_zx_switch` component

| Topic | Stock | Component (current) |
|---|---|---|
| Edge used | falling edge only (1 per period) | `ANY` (both flanks) is the measured default |
| Frequency | read from BL0942 FREQ register, once at init | estimated from the edge spacing (EMA), continuously |
| Frequency window | 45..65 Hz | 45..65 Hz (same) |
| `ht` | fixed after init | follows the estimate |
| Delay formula | `ht - (t % ht)` | same |
| Timers | 2 chained one-shot timers per edge (smaller delay first, second relative); handles ON and OFF requests of **several outputs** in one half period | one timer, one pending request (single relay) |
| Request semantics | newest request per output replaces older ones; waits for next edge | same (single output) |
| No-signal fallback | 2 mains periods without edge -> re-write OT_FUNX up to 3x -> flush requests unsynchronised | 60 ms request timeout -> immediate switch; estimator unlock after 3x max half period |
| Relay times | 6500 / 7800 us (shared by Plug PM G3 and Power Strip G4) | same defaults |
| OTA | synchronizer disabled during OTA | not handled |
| Start of ZX mode | when chip init is done | 3 s after boot, refresh every 60 s |

## 5. Ideas for extending the component (not done yet)

- **Several relays on one ZX source** (Power Strip Gen4: several outputs sharing one synchronizer; **inferred** from the request list with `capacity=3` and the `chid:%d Pending requests` log in the Gen4 code, not verified against the hardware): keep one
  estimator + one timer chain per edge, a request list per output, and the "smaller delay first" ordering of section 2.1.
  The current class owns the timer; a shared "zx hub" component with the estimator/timer and per-relay `register_output()` would map to it.
- Optionally use **FALLING only** to match stock exactly (one reference edge per period, `ht` from the frequency).
- Optional watchdog that re-writes OT_FUNX after 2 missed periods (instead of only every 60 s).
- Open questions that need a scope or a device, not more disassembly: real contact timing (6500/7800 are Shelly's numbers, the
  sync target is the **contact movement**), the phase of the BL0942 ZX edge relative to the true zero crossing (stock assumes 0),
  whether the Gen4 relay type really equals the Plug's.

## 6. Open points in the reverse engineering

- `ZXSyncNonNativeGpio` (ZX via a non-chip GPIO) exists in the code but is **not instantiated** in the Plug PM G3 init (only `ZXSyncNativeGpio`
  at 0x42007a56). Not analysed in detail; may be used on other hardware (Gen4 init uses one constructor call as well).
- The hw-timer flag `0x10000` is not decoded (probably "run callback in ISR / IRAM").
- `0x4220800a` is identified as `roundf` from its code, not from a symbol.
- The Gen4 build was only checked at the level of "same strings, same constructor values, same timer A/B pair"; its request records are 0x1c bytes
  (Plug: 0x28) and signed compares are used; the algorithm is the same.

## Tools (`__Idea/tools/`)

- `esp_image.py` - ESP app-image loader (virtual address <-> file offset), RISC-V disassembly helper (capstone, skipdata on).
- `xref_index.py <dump> <app_base> <out.pkl>` - builds `{target address: [instruction addresses]}` from `lui/auipc+addi/lw/sw` pairs; used to find string references.
- `dis_range.py <dump> <app_base> <lo_va> <hi_va>` - annotated disassembly of a virtual address range (string and rodata hints).
  Example: `python dis_range.py ../../_Input_/Shelly_Dumps/shelly_plug_pm_g3_3.bin 0x3a0000 0x4215b3d6 0x4215b560`
  Call targets via `auipc+jalr` are printed as relative offsets: resolve with `address_of_auipc + (imm<<12) + offset`.
  ROM libgcc entries seen: `0x400007b0` __divdf3, `0x40000848` __muldf3, `0x400007cc` __extendsfdf2, `0x400007e8` / `0x400007dc` double->int, `0x400008c8` __unordsf2.
