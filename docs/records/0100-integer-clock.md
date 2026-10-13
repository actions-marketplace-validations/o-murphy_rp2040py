# 0100. An integer time base for the simulation clock

- Status: **Proposed - authorised by the maintainer on 2026-10-12; the unit is measured and chosen (2026-10-13); the clock core is built (phase A), the blocks' own time arithmetic is phase B.** A cross-block change; the order below puts it after the XIP_CTRL port and before the USB port.
- Conceived: 2026-10-12
- Related: [0096](0096-cpp-mcu-core.md) (the C++ `Clock` and every block that schedules an alarm on it), [0098](0098-datasheet-conformance-audit.md) (the section "A limit found on the way", which found it).

## The problem

The simulation clock counts nanoseconds in a `double` (`Clock::now_`, the Python clocks' `nanos`, every alarm's due time). A `double` is exact for integers only up to 2^53 ns, about **104 days** of simulated time; beyond that its resolution is 16 ns, then 32 ns, ... so `now + 8 ns == now` and every delay shorter than the resolution is silently lost.

It was found through MicroPython's own test `ports/rp2/rp2_dma.py`: it passes alone, in pairs with each earlier file and on a fresh emulator, and fails after the whole `extmod/machine_*` sequence. After those files `clock.nanos` is about `7.8e16` (2.5 years of simulated time: an earlier file leaves a very long idle jump behind), and a diagnostic print showed consecutive DMA transfers with `now == last` - the DMA's one-word-per-cycle schedule (`+8 ns`) collapsed to a single instant. The DMA is right; the time base is not. **Anything with a short delay is exposed**: UART and SPI frame times, the ROSC counter, PWM and TIMER ticks, PIO pacing.

## The decision

An **integer time base**: `int64` ticks in the C++ `Clock` (and the Cython `SimulationClock`) and in the Python clocks (`SimulationClock`, `MockClock`), instead of `double` nanoseconds. Fractional delays (a UART bit time, a pacing period, `1e9 / clk_sys`) are converted to ticks once, at the edge where a block schedules.

**Order (decided 2026-10-12):** ROSC (done) -> IO_BANK0 + PADS_BANK0 -> XIP_CTRL -> **this** -> USB -> the gate review. The first two ports have almost no timers, so doing them on the old clock costs nothing; USB has the most time-driven alarms (SOF, packet delays) and should be ported on the new clock rather than converted afterwards.

## To decide when it starts (not decided here)

- **The unit.** It must hold years without overflow and represent the common periods exactly. `int64` picoseconds overflow after ~106 days (no better than the problem); 1/16 ns (62.5 ps) lasts ~18 years and makes a 125 MHz cycle exactly 128 ticks, but not 12 MHz; whole nanoseconds last 292 years but lose the sub-nanosecond periods (a 125 MHz cycle is 8 ns, a 48 MHz cycle 20.83 ns). Possibly nanoseconds in `int64` plus a sub-nanosecond remainder per alarm user. To be chosen by measuring what the blocks need.
- **The public surface.** `clock.nanos`, `tick(delta)`, `schedule(alarm, delta)`, `nanos_to_next_alarm` are read by tests, oracles and every block; whether they keep their names (as floats, derived) or change.
- **Rounding** of the fractional periods, and keeping the two implementations (Python reference and C++) identical to the last tick, since the oracles compare time.

## Measurements (2026-10-13; `scripts/audit/clock_units.py`)

The script computes, on exact fractions, the rounding of the periods the blocks schedule (a CPU cycle at 125/133/200/250/266/48/12/6.5 MHz, UART frames from 9600 to 3 Mbaud, SPI bytes, a DMA word, the RTC second, the TIMER tick, the XIP stream word, the ROSC cycle, a WS2812 bit) for candidate units.

- **Range of an `int64`:** 1 ns 292 years; 1/16 ns 18.3 years; 1/32 ns 9.1; 1/64 ns 4.6; 1 ps 0.3 years. The failing official-suite sequence reaches 2.5 years of simulated time (`7.8e16` ns).
- **What is exact on every unit:** 125/200/250 MHz cycles, a 1 Mbaud frame, SPI bytes off 125 MHz, the RTC second, the TIMER tick, a WS2812 bit. Frame-sized periods (UART, SPI) are within 0.0003 % on 1/16 ns.
- **What is not:** only **the period of one cycle at a non-integer clock** - 133, 266, 48, 12, 6.5 MHz. Rounded on its own, each cycle is off by -18.8 ps on 1/16 ns (-2500 ppm at 133 MHz), -9.4 ps on 1/32 ns, -3.2 ps on 1/64 ns, and by 481 ps (6.4 %) on whole nanoseconds. A finer unit shrinks this and does not remove it, while it costs range.

**Decision:** `int64_t` ticks of **1/16 ns**; `long` is not used (32 bits on Windows and 32-bit ARM, both in the CI wheel matrix). The error that matters is cured by **carrying the remainder** where a block schedules per cycle - the CPU batch already rounds once per batch (`cycles x period`), the DMA (one word per cycle) and the other per-cycle schedulers keep a fractional accumulator so the long-run rate is exact. Overflow is not silent: times saturate at `INT64_MAX / 2` and the clock reports it. 1/32 ns (9.1 years, 3.7 x the observed 2.5) is the fallback if more resolution is wanted.

## Scope

The C++ `Clock` and alarm list, `SimulationClock` (Cython) and the Python clocks; every `schedule()` caller (UART, SPI, ADC, DMA's `transfer_ns`, ROSC's COUNT, PWM, TIMER, WATCHDOG, RTC, PIO pacing, the batch loop's `delta_nanos`); the oracles' time snapshots; the benchmark (an A/B against the commit before - the clock is on the hot path).

## Done when

`ports/rp2/rp2_dma.py` passes in the full official-suite sequence; a test advances the clock past 2^53 ns and still resolves an 8 ns step; the oracles and the C++ checks pass on both builds; the benchmark is not slower beyond noise.

## Progress log

- 2026-10-13: **phase A built: the clock core is integer ticks.** C++ `Clock` (`core/clock.hpp`): `int64_t` ticks of 1/16 ns (`ticks()`, `tick_ticks`, `link_ticks`, `schedule_ticks`, `ticks_to_next_alarm`), `nanos_to_ticks` (round half up, saturating at `INT64_MAX / 2`, NaN as 0), `advance_cycles` (the remainder of a cycle that is not a whole number of ticks carried), `saturated()`; the `double`-nanosecond entry points stay for the blocks and convert once. The Python reference and the Cython `SimulationClock` mirror it (`ticks`, `tick_ticks`, `advance_cycles`, `ticks_to_next_alarm`, `ClockAlarm.schedule_ticks`/`.ticks`; `nanos` is derived). The batch (`batch.hpp`, `_execute_batch.py`) advances in cycles and keeps its budget in ticks, rounded up. The DMA keeps a per-channel remainder. Tests: `tests/cpp/test_clock.cpp`, `tests/test_simulation_clock_parity.py` (an 8 ns step after 2^53 ns, the 7.8e16 ns reading of the failing run, rounding and saturation, 133 MHz without drift), `tests/test_dma.py`. Benchmark (`scripts/bench/synthetic.py`, best of 5, A/B against the commit before, same machine): before 111 and 126 Minstr/s, after 86, 130 and 132 - within the noise, once the batch took an integer fast path for a clock that is a whole number of ticks per cycle (a first version that always went through the floating-point remainder was 15-25 % slower). Phase B (open): the blocks that subtract `clock.nanos` (TIMER, ROSC COUNT, TIMER32, the XIP flush deadline, WS2812, the pacing in `simulator.py`) should take their differences in ticks; they are exact below 2^53 / 16 ns (104 days) and lose sub-16-ns resolution above it, which none of them needs but the oracles' snapshots will want exact.
- 2026-10-13: **unit measured and chosen** (above): int64 ticks of 1/16 ns, remainder carried by per-cycle schedulers, saturating overflow. Nothing built yet.
- 2026-10-12: proposed; authorised by the maintainer; ordered after XIP_CTRL and before USB. Nothing built.
