# 0104 - the second core: the audit of the seams, and an architecture that costs nothing while core 1 is off

Status: proposed (2026-10-18), **documentation only - nothing is built, and the scheduling architecture is not chosen.** Companion to [0053](0053-core1-and-inter-core-fifo.md) (the second core and the inter-core FIFO; its addendum settles the execution model), to the "Dual-core seams" note in [0096](0096-cpp-mcu-core.md) and to [0102](0102-c-abi-for-the-core.md) / [0103](0103-wasm-backend.md) (the C ABI and the wasm backend, which must not freeze a single-core shape). Per `CLAUDE.md` a note about what should eventually happen is not a go-ahead to implement it: each phase below needs its own "yes".

## Why now, and the change of order

Until 2026-10-18 core 1 was "deferred" (0096, the gate review: `multicore_launch_core1` must fail loudly). The maintainer's call of 2026-10-18 makes it a requirement: MicroPython's `_thread` on the RP2040 runs the thread on core 1 through `multicore_launch_core1`, and the latest CoreMark for the RP2040 (releases 1.0.4 to 1.0.7) splits its iterations across both cores - `demo/coremark_single_thread.py` is pinned to the single-core v1.0.0 for exactly that reason and says so.

So the order of 0096's Phase 6 / 0103 changes. It was: the Python layer of 0103 (`RP2040PY_BACKEND`, `--backend`), then widening the C ABI of 0102, then core 1 "later". It is now:

1. **This record's audit** (done below, 2026-10-18) and the architecture (below).
2. **Phase P0, the seams, with no change of behaviour** (below) - before the C ABI of 0102 is widened, because the ABI is where a single-core shape would be frozen (`rp2040_register(index)` has no core argument today) and because it costs least while the ABI has only internal users.
3. **Phase P1/P2, core 1** (C++, then the Python reference, the oracle and the shells).
4. **Then** the Python layer of 0103 and the widening of the ABI - with a core argument from the start. The *selection* of a backend (`RP2040PY_BACKEND`, a `--backend` axis in `conftest.py`) does not depend on core 1 and may be done earlier if wanted; only widening the ABI waits for P0.

## The requirement that shapes the design

**While core 1 is not running, the emulator must cost exactly what it costs today** - not "a few per cent", nothing: the machine code of the single-core batch loop must be the same. Everything below is chosen for that, and P0 has an acceptance test for it (below).

## What is decided, and what is not

**Decided by the maintainer (2026-10-18):**
1. The change of order above.
2. **While core 1 is off the emulator must cost what it costs today, ideally exactly** - the performance requirement above.
3. **No OS thread per core.** Both cores run on the thread that pumps the engine (0053's addendum has the reasons: one shared clock couples every instruction, and determinism).

**Not decided - and the maintainer does not yet know which is more efficient:** *how the two cores are scheduled on that one thread* - alternating, an async / coroutine style, or something else. Everything under "Proposals" below is **my proposal, not a decision**, and the scheduling question has its own section ("Open: how the two cores are scheduled") with the candidates and how to choose by measurement. The seams (P0) do not depend on the answer; the dual loop (P1) does.

## The audit

What a second core touches today (counts and places are from the tree at `830244d`).

| Piece | Where | Today | What core 1 needs |
|---|---|---|---|
| The CPU | `core/cpu.hpp`, `Cpu` (240 bytes) | registers, flags, cycles, **the whole NVIC state** (`interrupt_priorities`, pending/enabled sets, NMI mask), the event register and `waiting` all live in `Cpu`; the opcode table `table_` is a pointer to one static table | a second `Cpu` object is a second 240 bytes and shares the table. **Seam 3 of 0096 is already mostly true.** |
| The bus | `core/bus.hpp` | one `Bus`; SIO and the PPB have their own handler slots (`sio_`, `ppb_`, `has_sio_`, `has_ppb_`) tried before the window table | the two slots need to know *who* is accessing; RAM, flash and the 40-odd other windows do not |
| The PPB | `core/ppb.hpp`, one `PpbBlock` with `Cpu* cpu_` | SysTick (a timer and its alarm), SCR, the MPU registers; NVIC reads and writes go to `cpu_` | one per core (SysTick, SCR, MPU and the NVIC window are banked) |
| SIO | `core/sio.hpp` | one `SioBlock`: spinlocks, GPIO, the **divider**, **two interpolators**, `CPUID` constant 0; the FIFO registers warn (`kSioWarnFifo`) | per core: `CPUID`, divider, both interpolators, the two FIFOs and their IRQ lines; shared: spinlocks, GPIO |
| Cycles charged by SIO | `Mcu::sio_add_cycles` (`mcu.hpp:801`) | `cpu.cycles += ...` | the cycles of the *initiating* core |
| IRQ delivery | `Mcu::set_interrupt` (`mcu.hpp:527`), reached by every block through `ChipPorts::irq` | one line into `cpu` | the same line into both NVICs (the chip routes every peripheral IRQ to both cores); IO_BANK0 has separate PROC0 / PROC1 `INTE` / `INTF` / `INTS` sets and so a *different level per core*; the SIO FIFO IRQs are per core (their exact routing to be read in datasheet 2.3.2 - **not yet verified**) |
| SYSCFG | `core/syscfg.hpp` | `PROC0_NMI_MASK` is the C++ `Cpu`'s own `interrupt_nmi_mask`; `PROC1_NMI_MASK` is stored | `PROC1_NMI_MASK` to core 1's `Cpu` |
| `SEV` | `cpu_ops.hpp:522` | sets the event register of the executing core only (the comment says "every PE": true with one) | every present core |
| The batch loop | `core/batch.hpp`, `run_batch(Cpu&, Clock&, ...)`, one caller, `Mcu::run` | one core, one instruction per iteration, the clock ticked per instruction (`tick_batch == 1`), PIO stepped by the instruction's cycles | a loop for two cores sharing one clock; PIO and DMA stepped by the *clock's* delta, not by one core's cycles |
| Reset | `Mcu::reset`, PSM, RESETS | PSM has no PROC1 handling; `leave_reset` puts core 0 at the flash entry | core 1 held (inactive) until launched; `PSM.FRCE_OFF` / `FRCE_ON` for `PROC1` |
| The Python reference | `_rp2040.py`, `_execute_batch.py`, `_sio.py`, `peripherals/_ppb.py`, `peripherals/_syscfg.py`, `device/base_device.py`, `cli/__init__.py`, `gdb/gdb_server.py`, `native/_rp2040.pyx`, `_ppb.pyx`, `_sio.pyx` | **33 uses of `.core` in 11 files**; one `core` | `core` stays core 0 (no API break); a `cores` sequence; the reference has the same dual loop (the oracle needs it) |
| The host callbacks | `McuHost` (`on_break`, `log`, `bl_taken`) | no core in them | a core index (a debugger needs to know which core hit a breakpoint) |
| The C ABI | `native/wasm/rp2040_wasm.cpp` | `rp2040_register(index)`, `rp2040_set_register(index, value)` | `(core, index)`; `rp2040_run*` runs the chip, not "the core" |

What is **not** affected: the memory map and the 40-odd peripheral windows (shared by both cores, reached the same way), the clock and its alarms, DMA, PIO, the pin banks.

## Proposals (none of these is decided)

D1-D5 and D7-D11 are the parts that hold whichever way the scheduling question is answered; D6 is replaced by the open section after them.

**D1 - a second `Cpu` in the `Mcu`.** `Mcu::cpu` stays core 0 (every caller and the whole public API unchanged); `Mcu::cpu1` is added, `present` from construction but **inactive** until launched. No pointer is added to the single-core path.

**D2 - the initiator is one byte on the `Bus`, set when the scheduler switches core, read only by SIO and the PPB.** `Bus::initiator` (0 or 1). The scheduler writes it *when it changes core*, not per access; the SIO and PPB handler slots - already special-cased and tried before the window table - index their per-core state with it. RAM, flash and every other window never look at it: **zero cost for them**. Rejected: a core argument in every `BlockWindow` handler (a signature change in all blocks and the Cython trampolines, for two blocks that need it); a `Bus` object per core (an extra indirection on the hottest path, the memory map).

**D3 - the PPB and the per-core SIO state are `[2]`.** `PpbBlock ppb_[2]` (each owns its SysTick, SCR and MPU, and points at its own `Cpu`); `SioBlock` gets a `PerCore core_[2]` (divider, interpolators, FIFO ends); the bus slot's handler picks `[initiator]`. With one core the index is the constant 0 and the cost is one indexed load on an SIO or PPB access, not on any other access.

**D4 - IRQ delivery is by core.** `ChipPorts::irq(line, level)` keeps its signature for the blocks that do not care (every peripheral: delivered to both NVICs, which is what the chip does; core 1's pending bits are therefore faithful from reset); a per-core entry (`irq_core(core, line, level)`) serves IO_BANK0 (a level per core from `PROC0_INTS` / `PROC1_INTS`) and the SIO FIFO lines. IRQ events are rare (not per instruction): the second store costs nothing measurable.

**D5 - two batch loops, chosen once per batch.** `run_batch` (the single-core loop) is **not touched**. A separate `run_batch_dual` serves the time core 1 is running; `Mcu::run` chooses between them at the start of a batch (one cold branch per batch, not per instruction). Switching *during* a batch reuses the exit flag the loop already tests every iteration (`clk_changed_`, the one that ends a batch when `clk_sys` changes): launching core 1 raises it, the batch returns, the next `run` picks the dual loop. The single-core loop therefore gains no instruction.

**D6 - the scheduling of the two cores: open** - see the next section.

**D7 - core 1 is launched by a high-level emulation of the boot ROM's handshake.** Core 1 stays inactive (and free) until core 0 pushes the six words `0, 0, 1, vector_table, sp, pc` through the SIO FIFO, in that order (the sequence `multicore_launch_core1` and MicroPython's `_thread` use); then `cpu1` is reset, given the stack pointer and the entry point (and its VTOR), marked active, and the batch-exit flag raised. The real boot ROM on core 1 (exact, but needs the ROM image and a reset flow the emulator skips today - `leave_reset` goes straight to the flash entry) is a later option, not the first step. `multicore_reset_core1` and the FIFO drain / `PSM` `PROC1` reset return core 1 to inactive.

**D8 - the FIFOs are what the datasheet says.** Two 8-entry, 32-bit FIFOs (core 0 to core 1 and back); `FIFO_ST` (`VLD`, `RDY`, `WOF`, `ROE`), `FIFO_WR`, `FIFO_RD`, the per-core IRQ; the stub warning (`kSioWarnFifo`) goes away. Spinlocks stay shared; `CPUID` reads the initiator; the divider's and the interpolators' cycles are charged to the initiating core.

**D9 - the Python reference gets the same loop.** The pure-Python chip is the second implementation and the oracle's other side: `_execute_batch.py` and `_rp2040.py` grow a dual loop used only once core 1 is launched, so the reference too keeps its single-core path byte for byte. `rp2040.core` stays core 0; a `cores` sequence and `core1` are added. The lockstep oracle runs a two-core Thumb program on both and compares registers, memory and cycles of both cores.

**D10 - the C ABI takes a core.** `rp2040_register(core, index)`, `rp2040_set_register(core, index, value)`, `rp2040_core_active(core)`; `rp2040_run` / `rp2040_run_n` run the chip (both cores if active). To be done as part of widening the ABI, after P0 (0102 notes it).

**D11 - an option, not decided: a compile-time `RP2040PY_SINGLE_CORE`** that removes the dual loop and core 1's state, for builds where size matters (the wasm module is 187 KB today). Off by default; to be decided with the size of the dual code in front of us.

## Open: how the two cores are scheduled on one thread

The problem: the cores share one clock, and every instruction of either can change what the other sees (a peripheral register at time `t`, a FIFO, a spinlock, shared RAM). The scheduler decides in which order the instructions of the two cores run, and when the clock, the alarms, the PIO and the DMA are advanced. The choice sets the cost per instruction, the accuracy of timing, and the determinism. **Nothing here has been measured with two cores.**

| Candidate | How | For | Against / unknown |
|---|---|---|---|
| **A. Local-time scheduling** | each core has a local time; run the one with the smaller time, ticking the clock to it first | exact: no skew, an alarm or a PIO step is never late for either core; deterministic (ties to core 0) | a compare and a clock tick per instruction (the tick is already per instruction today); a core switch almost every instruction when the cores run at the same rate |
| **B. Strict alternation** | one instruction each, in turn | the simplest | not time-faithful: a 3-cycle instruction on one core and a 1-cycle one on the other would not interleave by time |
| **C. Quantum** | run each core for N instructions (or until the next alarm), then switch | few switches; the clock ticks once per quantum instead of per instruction - possibly **cheaper than today per instruction**, which would pay for the second core | an alarm, an IRQ or a PIO step can be late for the other core by up to N; N has to be chosen; PIO timing is visible to firmware |
| **D. Cooperative / coroutine** | each core a resumable task, a scheduler picks by time (the form A or C takes in an `async` host, or in the Python reference as generators) | fits the `asyncio` model of 0025 / 0026 on the Python side | not a different semantics from A or C, a different mechanism; per-instruction `await` is far too slow in Python, so it can only sit at the quantum boundary |
| **E. Run until a shared access** | run a core until it touches something the other can observe (the SIO FIFO, `SEV`, a spinlock, an alarm due), then switch | the fewest switches | shared RAM is observable too (a flag polled in RAM) and cannot be detected for free, so a bound on the run is still needed: it falls back to C |

**My hypothesis, untested:** C (a quantum bounded by the next alarm) is likely the cheapest and A the most faithful, and the difference may be small; but I do not know, and the answer depends on how often real firmware (MicroPython's `_thread`, CoreMark) lets one core wait for the other.

**How to choose - by measurement, before P1:** a throwaway prototype of the candidates in the bare-core harness (a scratch file, not in the product tree), driven by (1) two copies of the synthetic loop of `scripts/bench/synthetic.py` on both cores (the cost per instruction against the single-core 59 Minstr/s of the C++ build), (2) a FIFO ping-pong between the cores (the cost when they wait for each other), (3) core 0 busy and core 1 waiting (the cost of the idle one), (4) the accuracy side: a PIO program and a timer alarm whose timing a core observes. The numbers go here; then D6 is decided. Whichever candidate wins, D5 holds: a separate loop for two cores, chosen once per batch, with the single-core loop untouched.

## Phases

- **P0 - the seams, no change of behaviour.** `Bus::initiator`, `Mcu::cpu1` (present, inactive), `ppb_[2]`, SIO per-core arrays, the per-core IRQ entry, `SEV` over the present cores, the core argument in the C ABI accessors. Every existing test passes unchanged. **Acceptance (all four):** (a) the callgrind instruction count of `run_batch` over the synthetic program of `scripts/bench/synthetic.py` is **identical** to the build before P0 (it is deterministic: the count, not a timing, is the proof of "nothing added"); (b) the synthetic benchmark of `scripts/bench/wasm_core.py` is within the noise (+-2 % best of 5) on the Cython build, the C++ build and the wasm module; (c) the CoreMark demo's wall time is within +-3 % (47 s for 12.07 simulated seconds on the 2.8 GHz Xeon of the sandbox of 2026-10-18); (d) the wasm module's size is reported (187 KB before).
- **P0.5 - the scheduling prototype and the decision.** The throwaway prototype of the candidates above, the numbers in this record, then D6 decided by the maintainer. No product code.
- **P1 - core 1 in C++** (after P0.5). the dual loop (the candidate chosen in P0.5), the FIFOs, the launch handshake (D7), `PSM` / `RESETS` `PROC1`, IO_BANK0 `PROC1`, `PROC1_NMI_MASK`. Tests in the bare-core harness with small hand-assembled two-core programs: FIFO ping-pong, spinlock contention, `SEV` / `WFE` wake, an IRQ on each core, the launch and the reset of core 1. **Acceptance:** P0's four, unchanged, with core 1 never launched; and the cost with two busy cores measured, expected near 2x the single-core wall time per simulated second (two cores execute twice the instructions).
- **P2 - the reference, the oracle and the shells.** The dual loop in Python, the lockstep oracle, the Cython shells (`cores`, `core1`), the `Simulator`, the GDB stub's core selection.
- **P3 - real firmware.** MicroPython `_thread`, pico-sdk's multicore examples (the Pico SDK workflow), CoreMark 1.0.4 or later (the demo can move from the pinned v1.0.0; **the README is not touched until the maintainer says**).

## How to check it: the official MicroPython suite

The maintainer's suggestion of 2026-10-18: extend the coverage of MicroPython's own test suite, in CI or locally. It already runs in CI as groups of `tests/micropython_official/run_suite.sh` in `.github/workflows/ci-micropython-official.yml` (`ports/rp2` and `extmod/machine_*`, `micropython/`, `extmod/`, `basics/`, each report-only or `STRICT`). The addition is a group **`thread/*.py`** (MicroPython's `tests/thread`), run **report-only first** - today they cannot pass, and the baseline (which hang, which fail, which skip) is worth recording *before* P1 so that P1's effect is a measured difference - and promoted to `STRICT` for the files that must pass once core 1 works (the ones that need real parallelism or timing the emulator does not give go to `EXCLUDE`, with the reason). Also candidates: `ports/rp2` tests that touch `_thread` or the FIFO. The baseline run is a small local step and does not need core 1.

## Risks and open questions

1. **The IRQ routing per core** (SIO_IRQ_PROC0 / PROC1, and which lines each core sees) must be read in datasheet 2.3.2 and the SIO chapter before P1; the table above says "not yet verified" where it is.
2. **Time with two cores in the PIO and the DMA** (D6): the reference steps them by "the instruction's cycles"; stepping them by the clock's delta must give identical results in the single-core case - a test for P0/P1 before the dual loop is trusted.
3. **Idle with one core waiting** and the other running (the existing idle jump is for one core).
4. **The debug hooks** (`on_break`, `log`, `bl_taken`, the GDB stub): which core.
5. **The wasm module's size** with the dual loop and core 1's state (D11).
6. **The speed of two busy cores** is not yet measured; 0096 has only the 1.34x real time for one busy core. The backends' figures of 0103 are to be re-measured after P1.
7. **Whether the real boot ROM should run on core 1** (exact) instead of the handshake emulation (D7) - not needed for the first release of core 1.
8. **Determinism** is kept (one thread, ties to core 0); free-threaded builds are unaffected (no thread of ours).

## Progress log

- 2026-10-18: written (documentation only): the change of order, the audit of the tree, proposals D1-D11, the phases and their acceptance, the check through MicroPython's own suite. **Corrected the same day on the maintainer's word:** the scheduling of the two cores had been written as decided (local-time, one after the other); it is not - only "no OS thread" is. It is now an open question with candidates and a measurement plan (P0.5).
