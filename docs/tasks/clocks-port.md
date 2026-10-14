# CLOCKS and PLL: audit against the datasheet, then the C++ port (working note)

Status: audit read, decisions made, reference fixed (2026-10-13, `2c90ed2`), `clocks.py`/`pll.py` are facades over `_clocks.py`/`_pll.py`, C++ `core/clocks.hpp` + `core/pll.hpp` (mutation-tested) and the native shells in, oracle `tests/test_clocks_diff.py` (63 reference mutants) agrees; CPU time follows clk_sys (live boot: 70 252 us at 125 MHz, 35 125 us at 250 MHz); C++-to-C++ sink wiring (PPB, PWM, watchdog, RTC; the host's listeners, the chip's clk_sys/clk_peri and the UART baud announcement stay Python), XOSC state-change retune and live boots (MicroPython 1.29.0, CircuitPython 10.2.1, Kaluma 1.2.1, pico-sdk 1.2.0 hello_usb) done; open: the 0098 section. Recipe: [core-block-recipe.md](../reference/core-block-recipe.md), section A first. Folded into record 0098 (a CLOCKS/PLL section, as for USB) when done.

Why this is in the C++ plan at all: record 0096's rule (2026-10-06) is that the core owns every behaviour firmware can observe with no host attached, and it lists the clock tree among the debt (item 2): "a core that is to run alone has to own `clk_sys`/`clk_peri` itself - the DMA already asks the host for `clk_sys` through a callback, which a headless host cannot answer". The 2026-10-05 entry that left the tree in Python "on purpose" predates the rule.

Sources read: RP2040 datasheet 2.15 (clocks: 2.15.1-2.15.6, tables 205-252) and 2.18 (PLL, tables 274-278), pico-sdk `hardware_clocks`/`hardware_pll` (`clock_configure`, `pll_init`, `frequency_count_khz`), `python scripts/audit/datasheet_conformance.py rp2040.txt CLOCKS` / `PLL` (48 + 4 registers: only `ENABLED0/1` missing; the PLL notes are the usual placeholder-reset false alarms).

## What the model is

`peripherals/clocks.py` (`RPClocks`: the registers, the frequency of each generator from its mux and divider, the frequency counter FC0) and `peripherals/pll.py` (`RPPLL`: `f = xosc / REFDIV * FBDIV / (POSTDIV1 * POSTDIV2)`, BYPASS gives `xosc / REFDIV`). `update_clocks()` (in `clocks.py`, called by both chip classes through `RP2040.update_clocks()`) re-derives `clk_sys` and `clk_peri` after every relevant write and retunes the consumers: `ppb.clk_sys_changed` (SysTick), the PWM channel timers, the registered `add_clock_listener` callbacks (PIO dividers and the like), every UART (`clk_peri_changed`), `watchdog.clk_ref_changed` (the tick that drives the TIMER), `rtc.clk_rtc_changed`. `rp2040.clk_sys` / `clk_peri` / `xosc_freq` (12 MHz constant) / `rosc_freq` (the ROSC block pushes it) are plain attributes of the chip. A frequency of 0 (a stopped generator, an unmodelled source) keeps the last value and notifies nobody.

## Findings (datasheet / SDK vs. the reference), by decision

Fix in the reference first (each gets a test citing its source):
1. **The PLL's power bits do nothing.** `PWR` resets to `0x2D` (PD, DSMPD, POSTDIVPD, VCOPD all 1: table 276, "To save power set high when PLL output not required"); the reference computes a frequency whatever they say. A PLL with PD or VCOPD set has no VCO, one with POSTDIVPD set has no output unless BYPASS. The consequence in the model: `CLK_SYS_CTRL.AUXSRC = PLL_SYS` before `pll_init` already reports 125 MHz of PLL.
2. **`CS.LOCK` is always 1.** It is a status ("PLL is locked", RO reset 0): 0 while the VCO is powered down, 1 once PD and VCOPD are clear. `pll_init` clears them and then polls LOCK, so the polling sequence stays valid; the existing "do not disrupt a PLL that is already correctly configured" test (`pll.c`) reads LOCK as well.
3. **`ENABLED0` / `ENABLED1` (0xB0, 0xB4) are absent** (RO, reset 0, one bit per destination clock: tables 251, 252). Reading them warns.
4. Nothing else in the register layer differs: the masks and resets of every CTRL / DIV / SELECTED register, the AUXSRC encodings (REF: PLL_USB 0, GPIN0 1, GPIN1 2; SYS and PERI as in 0098), the INT-only dividers of clk_ref / clk_usb / clk_adc, `SELECTED` one-hot, `FC0_*` were read in 0098.

Document, do not implement (each needs its own go-ahead):
- the **resus** (2.15.5): needs a stopped-clock model; `CLK_SYS_RESUS_CTRL` is stored, `RESUS_STATUS` reads 0, `INTR` reads 0 (0098 backlog).
- the **time the generators take**: the glitchless mux "does not switch instantaneously" (SELECTED "may briefly show all-0s"), an aux clock needs 2 cycles of its source to stop and 2 to restart; the model switches at once and `SELECTED` follows `CTRL.SRC` immediately.
- **WAKE_EN / SLEEP_EN / ENABLED** as real clock gating (stored only), the fractional divider's jitter (the average frequency is used), duty-cycle correction.
- PLL constraints outside their range (reference 5-800 MHz, FBDIV 16-320, VCO 750-1600 MHz): the datasheet gives no behaviour for them, so none is invented (LOCK follows the power bits only).

Not independently sourced: BYPASS passing `xosc / REFDIV` rather than the undivided reference (the port of rp2040js 1.4.0 did it that way; the datasheet says only "the reference clock"); the 12 MHz XOSC and 6.5 MHz ROSC defaults.

## What a stand-alone C++ tree needs (design, nothing built)

- A `ClocksBlock` and a `PllBlock` (or one header with both - to be decided), the registers and the frequency derivation moved from Python, with the sources (`xosc`, `rosc`, the two PLLs, the GPIN stubs at 0) as inputs the tree reads from the other C++ blocks instead of the chip's attributes.
- The retune: today a Python function that calls into six blocks. In C++ the tree needs a `ClockSink` list (function pointers + ctx), like `RebaseListener`: SysTick, the PWM counters, the UARTs, the watchdog tick, the RTC, the PIO dividers, the DMA's pacing, the SPI/I2C/ADC rates, and the host's own listeners (`add_clock_listener`). The shells register the Python callbacks in the same list.
- `clk_sys` / `clk_peri` / `clk_ref` / `clk_rtc` / `clk_usb` / `clk_adc` as values the tree owns and every block reads through it - which is what makes the DMA's `clk_sys` host callback and the XIP's disappear.

## Decisions (the maintainer, 2026-10-13)

1. **Two headers**: `core/clocks.hpp` (`ClocksBlock`) and `core/pll.hpp` (`PllBlock`), siblings, as the module-layout rule says; the clocks block reads the PLLs' frequencies.
2. **The CPU's instruction time follows `clk_sys`.** "If the firmware asks for a frequency change it has to work" - the emulator does not run faster or slower in wall time; the *simulated* time of an instruction is `1 / clk_sys`, i.e. what the firmware expects (an overclock to 250 MHz makes a bit-banged protocol take half the simulated time, an underclock to 12 MHz ten times as much). Today it is a constant 125 MHz (`CYCLE_NANOS` in `_simulator.pyx` and its pure twin). The integer clock already carries a fractional per-cycle period (`advance_cycles(cycles, ticks_per_cycle)`), so the mechanism exists; what changes is that the tree retunes it. A behaviour change: every test and CI job whose timing depends on a firmware that sets the frequency (the SDK's `runtime_init` sets 125 MHz, so the common case does not move; `machine.freq()`, `set_sys_clock_khz()` and the like do) is checked in the same commit.
3. **A disabled XOSC gives 0 Hz** (hardware-faithful: the PLL has no reference, `clk_ref` from XOSC does not run). The oscillator blocks are the tree's inputs (`XOSC.STATUS.STABLE`, `ROSC` frequency), not the chip's constants `xosc_freq` / `rosc_freq`. An experiment (2026-10-13, reverted, not committed) with 0 Hz for an unstable XOSC in the reference broke tests that select XOSC as `clk_ref` or feed a PLL without enabling the crystal first (`tests/test_clock_rebase.py` among them: the watchdog tick, hence the TIMER, derives from clk_ref); those tests will enable the XOSC as firmware does. A bare chip that never touches the tree keeps its 125 MHz default (the rule "a frequency of 0 keeps the last value" stays: the emulator starts as if the bootrom had set the tree up, which is what the SDK's `runtime_init_clocks` then re-does).

## Plan

1. This note + the tracker row; the PLL / ENABLED fixes in the reference with `tests/test_clocks_datasheet.py`, in their own commit.
2. `peripherals/clocks.py` -> `_clocks.py` + facade (and `pll.py` -> `_pll.py`); oracle `tests/utils/clocks_diff.py` with source-text mutants, covering the retune order as an ordered log of the sinks.
3. `core/clocks.hpp` (+ `pll`) + `tests/cpp/test_clocks.cpp` + `scripts/audit/cpp_mutants.py clocks`; shells; the chip's `clk_*` attributes become views of the tree.
4. Live boots (MicroPython `machine.freq()`, CircuitPython, Kaluma, the pico-sdk examples of the oldest SDK), docs, register coverage, tracker.

## Retune surface as it stands (read 2026-10-13, for the oracle and the C++ tree)

What the oracle has to log, in order, and what the C++ tree has to call. All of it is `update_clocks()` in `_clocks.py`; the reference calls it after every write to CLOCKS (CLK_REF/SYS/PERI/RTC CTRL and DIV, not USB / ADC / GPOUT), every PLL register write (`_pll.py`), and a ROSC change (`_rosc.py`, `native/_rosc.pyx`).

1. `clk_sys` changed (non-zero, differs): `_set_clk_sys` -> chip attribute, `ppb.clk_sys_changed`, every PWM channel timer (`pwm.clock_freq`), then the host listeners as `(new, old)`.
2. `clk_peri` changed (non-zero, differs): chip attribute, every `uart.clk_peri_changed()`.
3. **Always, changed or not:** `watchdog.clk_ref_changed(clk_ref)` and `rtc.clk_rtc_changed(clk_rtc)`. So the sink contract differs: sys / peri sinks are called on a change only, the watchdog and RTC on every retune (they compare themselves). The oracle must log the calls, not only the end state, so a candidate that "optimises" this away is seen.
4. `reset_clock_tree` (CLOCKS domain reset): both PLLs `reset()`, then clk_sys / clk_peri back to 125 MHz through the same two setters when they differ, then (reference line 582) the watchdog is told clk_ref again.

Already C++ and with their own `clk_*_changed`: watchdog (`native/_watchdog.pyx`), PPB (`_ppb.pyx`), RTC (`_rtc.pyx`), UART (`_uart.pyx`), ROSC, XOSC, PLL status read through `xosc_hz()`. So the C++ tree's sinks are calls between C++ blocks (function pointer + ctx, like `RebaseListener`), with the shells adding the Python host listeners to the same list. PWM still holds its timer frequency in Python (`channel.timer.frequency`), to be checked before the tree is ported.
The oracle's candidate is the facade (pure and native selected by `native_disabled`), its log the ordered sink calls; until the C++ tree exists it can only check the reference against its own mutants, which is the part that can be written now.
