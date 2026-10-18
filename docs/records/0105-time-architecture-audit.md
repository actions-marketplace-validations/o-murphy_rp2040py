# 0105 - the time architecture: an audit of the clock, the tick generators and who counts what

Status: **audit done (2026-10-18); one defect fixed (finding 1); the rest recorded with a proposed order, not started.** Prompted by the maintainer's remark that the clock and the generators "are all on crutches", and written before [0104](0104-second-core-seams-and-architecture.md)'s P0/D6, because the second core's scheduling contract depends on what "time" means for each block.

## What time is made of today (the same in the pure-Python reference and in C++)

- **The `Clock`** (`clock.hpp`, `clock/_simulation_clock.py`, the Cython `ClockAlarm` wrapper; `MockClock` for tests): an integer count of ticks of 1/16 ns, an ordered alarm list (FIFO on ties), `tick_ticks` fires alarms each at its own time, an origin that is rebased at `2^58` ticks with listeners for blocks holding absolute counts (clock, TIMER, TIMER32, ROSC, XIP_CTRL).
- **The batch loop** (`batch.hpp`, `_execute_batch.py`): runs one instruction, then advances the clock by that instruction's cycles (integer ticks per cycle when `clk_sys` gives a whole number, otherwise `advance_cycles` with a carried remainder); an idle (WFI) core jumps to the next alarm.
- **Three mechanisms side by side**:
  1. clock **alarms** (TIMER, UART/SPI/I2C bit times, DMA pacing, RTC second, USB frames, ADC, XOSC/PLL settling);
  2. **stepping in CPU cycles** handed out by the batch loop (PIO `advance(cycles)`, SIO divider/interpolator cycle charge `sio_add_cycles`);
  3. **frequency fan-out** by listener (`clk_sys_changed`, `tick_changed`, `clk_ref_changed`, `clk_rtc_changed`, `clock_sinks.hpp`): a block is told a frequency and keeps its own *continuous* model of a counter (`base + (clock.ticks - epoch) * rate`, as doubles) - TIMER, SysTick and PWM through `Timer32`.

## Findings

1. **TIMER alarm could fire before the count reached its target - fixed.** The wait was `to_uint32(target - micros())`, a truncation; with the count at a fraction of a microsecond (any PAUSE/unpause, or a tick change, at a non-whole time) the interrupt rose while `TIMELR` still read `target - 1`. Reproduced on both builds (a PAUSE at 0.5 us, ALARM0 = 100: INTR set with TIMELR = 99). The fraction is now kept, the wait is rounded **up** to a tick, and the alarm re-checks on firing (a remainder under one count re-arms it). Pure `_timer.py` and `core/timer.hpp` carry the same change; `tests/test_timer.py::test_alarm_never_fires_before_the_count_reaches_its_target`. The exact-integer cases (every previous test) are unchanged.
2. **PIO runs on its own cycle count, not on the clock.** The batch loop hands it `cycles` per instruction, and for an idle jump `cycles = int(idle_ticks / ticks_per_cycle)` (floored, minimum 1): the fraction is dropped on every jump, whereas the CPU's own time carries it (`advance_cycles`). On a non-whole-tick `clk_sys` (133 MHz is 120.30 ticks) a PIO behind many short idle jumps runs slow relative to the clock. By reading, not yet measured. With `RP2040PY_CLOCK_TICK_BATCH > 1` PIO is stepped by cycles the clock has not yet covered (off by default). Proposed: derive PIO's cycle from the clock (carry the fraction), or give the PIO an alarm of its own.
3. **Pin changes made by PIO are seen at the end of the instruction that stepped it**, not at the PIO cycle that made them: an external device attached to a pin reads `clock.ticks` after the CPU instruction, up to a few cycles later. By reading.
4. **A device access happens at the instruction's start, alarms fire at its end.** A `LDR` of `TIMELR` sees the time before the instruction's cycles; on silicon the data phase is at the end. Bounded by the instruction's length (1-3 cycles; longer for `LDM`/`PUSH`/`POP` of many registers). By reading; relevant to [0104](0104-second-core-seams-and-architecture.md)'s accuracy contract.
5. **A waking core is not aligned to its own clock.** An idle jump lands on the alarm's tick, so the first instruction after WFI starts at any tick, not on a `clk_sys` edge (up to one cycle). By reading.
6. **The tick generator is a frequency, not a pulse train.** On silicon the watchdog divides `clk_ref` into 1 us pulses and TIMER/SysTick count pulses; here they compute `count = floor/round(continuous time * rate)`, so the *phase* of the pulses is not modelled, and `Timer32` rounds half-up (`Math.round`, kept from rp2040js) so a counter value can lead the true value by half a count (SysTick on the 1 us reference reloads 0.5 us after it reached 0 in a probe). TIMER's count is a `double` of microseconds with a fractional base (finding 1 was one consequence); an integer count plus a carried phase would remove the class of bug.
7. **Three clocks, one contract.** C++ `Clock`, the Cython wrapper and the pure `SimulationClock` (plus `MockClock`) have to agree; `tests/test_simulation_clock_parity.py` holds them. Cheap now, but every new time rule (rebase, FIFO order, `advance_cycles`) is written three times.
8. **Rebasing is a global duty.** Every block holding an absolute tick must register a listener; the failure mode when one forgets is silent (record 0100's hang). Blocks holding only alarms are safe; PIO is safe because it counts cycles, not ticks.

## Proposed order (not started; each step needs its own go-ahead)

1. Finding 2 (PIO cycle derived from the clock, fraction carried) - the one with a measurable effect on a pin-level device; pure and native together, with the PIO oracle.
2. Finding 6, TIMER first: an integer count of ticks with a carried phase instead of a double of microseconds; SysTick/PWM `Timer32` after, keeping the `Math.round` parity with the reference until the reference is changed in the same step.
3. Findings 3-5 are modelling choices, not defects: write their bounds into the accuracy contract of 0104 and decide there whether any is worth removing.
4. Then 0104's P0 and D6.

## Progress log

- **2026-10-18** - audit; finding 1 reproduced and fixed in both builds with a regression test; the rest recorded.
