# 0104 - the second core: the audit of the seams, and an architecture that costs nothing while core 1 is off

Status: proposed (2026-10-18), **documentation only - nothing is built. The scheduling (F2 main, A strict) and the accuracy contract were decided by the maintainer on 2026-10-18; the rest of the proposals are not yet taken.** Companion to [0053](0053-core1-and-inter-core-fifo.md) (the second core and the inter-core FIFO; its addendum settles the execution model), to the "Dual-core seams" note in [0096](0096-cpp-mcu-core.md) and to [0102](0102-c-abi-for-the-core.md) / [0103](0103-wasm-backend.md) (the C ABI and the wasm backend, which must not freeze a single-core shape). Per `CLAUDE.md` a note about what should eventually happen is not a go-ahead to implement it: each phase below needs its own "yes".

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
4. **The scheduling of the two cores: F2 is the main mode, the exact scheduler A is the strict mode (2026-10-18).** F2 = a quantum bounded by the next alarm, in which every access that leaves RAM and flash (the SIO, the PPB, every peripheral) and the event instructions are synchronisation points ordered by time through a nested catch-up and a tick of the clock to the accessing core's time. A (exact, per instruction) stays selectable for the oracle and for timing studies. This was chosen on the prototype evidence below, not on a measurement of real firmware: the two-core firmware check (P3) can still show a case that needs A, and then the default is revisited.
5. **The accuracy contract is accepted** as written under "The accuracy contract this gives": exact for every device access, event instruction, alarm and interrupt; bounded by the quantum (default 16 cycles, a setting) for data two cores exchange through RAM or flash with no SIO or device access between; bus contention not modelled. The device-access-trace test (A against F2, equal) is an acceptance criterion of P1.

**Still not decided:** the other proposals D1-D5 and D7-D11 below remain **my proposals** until the maintainer takes them (D6, the scheduling, is now decided: item 4 above). The seams (P0) do not depend on the scheduler; the dual loop (P1) does, and is now F2. The section "Open: how the two cores are scheduled" keeps the candidates and the prototype evidence the choice rests on.

## The audit

What a second core touches today (counts and places are from the tree at `830244d`).

| Piece | Where | Today | What core 1 needs |
|---|---|---|---|
| The CPU | `core/cpu.hpp`, `Cpu` (240 bytes) | registers, flags, cycles, **the whole NVIC state** (`interrupt_priorities`, pending/enabled sets, NMI mask), the event register and `waiting` all live in `Cpu`; the opcode table `table_` is a pointer to one static table | a second `Cpu` object is a second 240 bytes and shares the table. **Seam 3 of 0096 is already mostly true.** |
| The bus | `core/bus.hpp` | one `Bus`; SIO and the PPB have their own handler slots (`sio_`, `ppb_`, `has_sio_`, `has_ppb_`) tried before the window table | the two slots need to know *who* is accessing; RAM, flash and the 40-odd other windows do not |
| The PPB | `core/ppb.hpp`, one `PpbBlock` with `Cpu* cpu_` | SysTick (a timer and its alarm), SCR, the MPU registers; NVIC reads and writes go to `cpu_` | one per core (SysTick, SCR, MPU and the NVIC window are banked) |
| SIO | `core/sio.hpp` | one `SioBlock`: spinlocks, GPIO, the **divider**, **two interpolators**, `CPUID` constant 0; the FIFO registers warn (`kSioWarnFifo`) | per core: `CPUID`, divider, both interpolators, the two FIFOs and their IRQ lines; shared: spinlocks, GPIO |
| Cycles charged by SIO | `Mcu::sio_add_cycles` (`mcu.hpp:801`) | `cpu.cycles += ...` | the cycles of the *initiating* core |
| IRQ delivery | `Mcu::set_interrupt` (`mcu.hpp:527`), reached by every block through `ChipPorts::irq` | one line into `cpu` | **datasheet 2.3.2:** every NVIC has the same interrupts routed to it, *except* the GPIO ones - `IO_IRQ_BANK0` (13) and `IO_IRQ_QSPI` (14) are one per bank *per core*, independent (IO_BANK0's `PROC0_*` / `PROC1_*` sets), so those two get a level per core. `SIO_IRQ_PROC0` / `PROC1` (15, 16) are ordinary system lines that reach both NVICs and carry the FIFO state of core 0 and core 1 respectively (the OR of that core's `VLD`, `ROE`, `WOF`). Every other line goes to both cores. |
| SYSCFG | `core/syscfg.hpp` | `PROC0_NMI_MASK` is the C++ `Cpu`'s own `interrupt_nmi_mask`; `PROC1_NMI_MASK` is stored | `PROC1_NMI_MASK` to core 1's `Cpu` |
| `SEV` | `cpu_ops.hpp:522` | sets the event register of the executing core (record 0050) | also the other core's: **datasheet 2.3.3** - the event signals are cross-wired, an event sent by one processor is received on the other; the flag is sticky (both `SEV` then both `WFE` wake at once) |
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

**D4 - IRQ delivery is by core only where the chip makes it so.** `ChipPorts::irq(line, level)` keeps its signature: every system line is delivered to both NVICs, which is what the chip does (datasheet 2.3.2), so core 1's pending bits are faithful from reset, and the FIFO lines 15 / 16 are ordinary lines driven by each core's `FIFO_ST`. Only `IO_IRQ_BANK0` and `IO_IRQ_QSPI` get a per-core entry (`irq_core(core, line, level)`), the level computed from `PROC0_INTS` or `PROC1_INTS`. IRQ events are rare (not per instruction): the second store costs nothing measurable. Each core also has its own NMI mask (`PROC0_NMI_MASK`, `PROC1_NMI_MASK`; the 26 lines are masked, ORed into the core's NMI).

**D5 - two batch loops, chosen once per batch.** `run_batch` (the single-core loop) is **not touched**. A separate `run_batch_dual` serves the time core 1 is running; `Mcu::run` chooses between them at the start of a batch (one cold branch per batch, not per instruction). Switching *during* a batch reuses the exit flag the loop already tests every iteration (`clk_changed_`, the one that ends a batch when `clk_sys` changes): launching core 1 raises it, the batch returns, the next `run` picks the dual loop. The single-core loop therefore gains no instruction.

**D6 - the scheduling of the two cores: decided (2026-10-18): F2 main, A strict** - see "What is decided" and the next section for the evidence.

**D7 - how core 1 is launched: two options, to be chosen.** What the chip does (datasheet 2.8.1 / 2.8.2): after reset *both* cores enter the boot ROM at the same place; both read `SIO.CPUID`; core 1 goes to sleep (`WFE` with `SCR.SLEEPDEEP`) and stays there until woken through the mailbox; the launch is a **lockstep, echoing state machine** over the two FIFOs - core 0 sends `0, 0, 1, vector_table, sp, entry` (draining its read FIFO and executing `SEV` before a `0`), core 1 echoes every word back, a wrong echo restarts the sequence, and it may be run at any point after core 1 has been reset. So a launch emulation must *answer*, not just watch. (a) **Run the real boot ROM on core 1** - exact, restarts and resets included, no emulation code; needs the ROM image (it is in the package, `BOOTROM_B1`) and `WFE` / `SLEEPDEEP` / `SEV` right; a host without the image (the wasm PoC passes none) cannot. (b) **A high-level responder in the SIO**: while core 1 is inactive, each word core 0 writes to the 0-to-1 FIFO is echoed into the 1-to-0 FIFO, a wrong word or a `0` restarts, the last word starts core 1 (reset, `VTOR`, `SP`, `PC`, active, the batch-exit flag raised). Likely both: (a) when the ROM is there, (b) otherwise. `multicore_reset_core1`, the FIFO drain and `PSM` (`FRCE_OFF` / `FRCE_ON` have a `PROC1` bit 16 next to `PROC0` bit 15) return core 1 to its boot state.

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
| **E. Quantum, ended early after an SIO access** | run a core until it touches the SIO (or the quantum ends), then let the other catch up | handoffs no longer wait for the quantum: the rate does not depend on it | **measured (below): not exact** - the other core sees the effect from its own past, a handoff rate 65 % above the exact one in the spinlock ping-pong |
| **F. Quantum, with every SIO access ordered by time** | a quantum cut at the next alarm; at an SIO access the *other core, if it is behind, first executes up to this core's time* (nested, inside the access), and only then does the access happen | **measured (below): the same result as A to the instruction**, independent of the quantum, at 1.4-1.6x A's throughput | shared RAM that two cores poll *without* the SIO is still delayed by up to a quantum; `SEV` / `WFE` need the same hook (two opcodes, not the hot path); the nested run needs care (different `Cpu` objects, shared bus and clock) |

**The first prototype round (below) found:** C is the cheapest and A the most faithful, and the difference is not small - exact scheduling costs 10-31 % per instruction, a quantum costs nothing but delays cross-core visibility in proportion to its length; E (below) was tried and is not exact; F (below) has both. The answer still depends on how often real firmware (MicroPython's `_thread`, CoreMark) synchronises through the SIO rather than through shared RAM.

**How to choose - by measurement, before P1:** a throwaway prototype of the candidates in the bare-core harness (a scratch file, not in the product tree), driven by (1) two copies of the synthetic loop of `scripts/bench/synthetic.py` on both cores (the cost per instruction against the single-core 59 Minstr/s of the C++ build), (2) a FIFO ping-pong between the cores (the cost when they wait for each other), (3) core 0 busy and core 1 waiting (the cost of the idle one), (4) the accuracy side: a PIO program and a timer alarm whose timing a core observes. The numbers go here; then D6 is decided. Whichever candidate wins, D5 holds: a separate loop for two cores, chosen once per batch, with the single-core loop untouched.

## Checked against the datasheet (2026-10-18, RP2040 datasheet, build 2026-10)

What the datasheet says that bears on this design, and what in this record it changed.

- **2.3.1 SIO.** Every IOPORT access takes **exactly one cycle**; a main-bus load or store takes two and may wait longer because of contention from other bus masters. **To check in the emulator:** how many cycles `execute()` charges for a load or store to the SIO window - not read yet. The contention itself is not modelled (0098 lists it) and will make a loaded dual-core run faster than silicon where both cores hit the same SRAM bank.
- **2.3.1.1 `CPUID`.** 0 on core 0, 1 on core 1; "both cores start running simultaneously".
- **2.3.1.3 spinlocks.** 32, shared; **if both cores claim the same lock in the same cycle, core 0 wins** - the tie rule used by the local-time candidate (ties to core 0).
- **2.3.1.4 FIFOs.** Two, 32 bits x 8 entries (core 0 to 1 and core 1 to 0). `FIFO_ST`: `VLD`, `RDY`, sticky `ROE` (read while empty) and `WOF` (written while full); a write when full or a read when empty changes no state. The IRQ of a core is the OR of its `VLD`, `ROE`, `WOF`; `ROE` / `WOF` are cleared by writing any value to `FIFO_ST`. The IRQs are system IRQs 15 and 16.
- **2.3.1.5 / 2.3.1.6.** One divider and two interpolators **per core**.
- **2.3.2 interrupts.** See the IRQ row of the audit: all lines to both NVICs, except `IO_IRQ_BANK0` / `IO_IRQ_QSPI`; an NMI mask per core; a caution that with the watchdog armed and bits in the core 1 NMI mask, `RESETS` (and so `SYSCFG`) belong in the watchdog reset list - the reset cascade of `Mcu` will need that.
- **2.3.3 events.** Cross-wired, sticky (see the `SEV` row).
- **2.8.1 / 2.8.2 boot and launch.** See D7: this **corrected** an earlier version of this record, which had the launch as "watch for the six words"; the handshake echoes.
- **PSM.** `FRCE_ON` / `FRCE_OFF` have `PROC1` (bit 16) and `PROC0` (bit 15): core 1 can be forced off and on.

Not yet read: 2.4 (the PPB is per core, SysTick included), 2.7 (the hardware reset sequence), the debug chapter (which core a debugger addresses), 2.11 (sleep: both cores asleep and the DMA idle lets the chip sleep, which is the idle jump with two cores).

## The prototype, first round (P0.5, 2026-10-18)

`scripts/bench/two_core/proto.cpp` (the harness: two `Cpu` on the `Mcu`'s bus and clock, one loop per candidate) and `gen.py` (the Thumb programs). Not product code; the loops are *minimal* (an instruction, the clock, no PIO / DMA / yield checks), so the absolute figures are higher than the product's (the single-core minimal loop is 83 Minstr/s where the product's `run_batch` is 59-60 on the same compiler): **read the ratios, not the figures.** 64 simulated ms per run (8 M cycles), best of 3, the sandbox's 2.8 GHz Xeon; "Minstr/s" counts the instructions of both cores. The baseline: one core, one clock tick per instruction (today's shape), **83.4 (g++) / 82.8 (clang)**.

| Candidate | two busy cores, g++ / clang | one busy, one asleep, g++ / clang | ping-pong through shared RAM: Minstr/s g++ / clang, and round trips per simulated ms |
|---|---|---|---|
| A local time, per instruction | 75.0 / 56.9 | 74.9 / 75.7 | 70.7 / 50.0, **6579** |
| A' local time, burst | 64.7 / 54.5 | 76.3 / 74.8 | 55.8 / 50.8, 6579 |
| B strict alternation | 91.4 / 97.6 | 69.7 / 80.6 | 94.3 / 92.2, 5682 - **wrong**: the cores' own times drift apart (up to 1.09 M cycles) |
| C quantum 16 cycles | 100.7 / 78.8 | 96.2 / 76.6 | 92.4 / 75.5, **7812** |
| C quantum 64 | 96.5 / 90.3 | 97.7 / 92.9 | 96.7 / 86.5, **2000** |
| C quantum 256 | 100.4 / 94.6 | 102.3 / 95.3 | 100.4 / 90.5, **500** |
| C quantum 1024 | 102.7 / 94.3 | 102.2 / 96.1 | 101.0 / 89.8, **125** |

What it says (and its limits):
1. **Exact scheduling (A) costs 10 % (g++) to 31 % (clang) per instruction over the one-core minimal loop**, and clang is the compiler the wasm module has to use. A' (running a burst) is *not* better than A when the cores run at the same rate: they alternate nearly every instruction anyway.
2. **A quantum (C) is cheaper than the one-core loop** (up to 1.2x), because the clock is ticked once per quantum instead of once per instruction - the batching the product turns off on purpose (alarm precision) pays here, since the quantum is cut at the next alarm and so alarms stay exact.
3. **But a quantum delays what one core sees of the other** in proportion to its length: the ping-pong rate is 6579 per simulated ms exact, 7812 at 16 cycles, and falls to 2000, 500 and 125 at 64, 256 and 1024. For firmware that polls shared RAM with no help from the SIO, a long quantum makes every hand-off cost a quantum of simulated time. The alarm probe (how far a core's time is from the alarm when it fires) reads <= 1 cycle for every candidate and does **not** tell them apart, because the quantum is cut at alarms - the cross-core delay is the real accuracy measure.
4. **An asleep core costs about 10 % under A** and nothing under C.
5. **B is out**: its time model is wrong.

**The second round: the SIO spinlocks as the synchronisation (W4).** Both locks held at the start; core 0 releases lock 1 and claims lock 0, core 1 claims lock 1 and releases lock 0 - the way SDK firmware hands work over (the FIFO is still a stub, spinlocks stand in). Throughput in Minstr/s (g++ / clang), the instructions of each core, and the round trips per simulated ms:

| Candidate | Minstr/s | core 0 / core 1 instructions | round trips per ms |
|---|---|---|---|
| A exact | 50.2 / 45.3 | 6133338 / 6266670 | **8333** |
| C quantum 16 / 64 / 256 / 1024 | 73.7 / 86.7 / 89.9 / 90.8 (g++) | | 7750 / 2000 / 500 / 125 |
| E quantum + cut after an SIO access | 63.4 / 60.2 | 6220005 / 6440005 | 13750 at every quantum |
| **F quantum + exact SIO ordering** (16 / 64 / 256 / 1024) | 61.6 / 67.3 / 68.2 / 67.5 (g++); 67.7 / 71.9 / 73.3 / 74.1 (clang) | **6133337 / 6266670** | **8333 at every quantum** |

**E is not the answer, F is:** E ends a core's run after an SIO access, but the other core was behind and replays its own earlier polls against an effect that happened later in time, so handoffs come too early (13750 against the exact 8333). F makes the access itself wait for time: the core that is ahead lets the other catch up *before* the access, so every SIO access is ordered by time, and the result is the exact scheduler's - the same instruction counts to within one, the same 8333 round trips - for any quantum, at 1.36x (g++) to 1.64x (clang) A's throughput. F's cost is the cost of the catch-up, which is work the other core had to do anyway; a spinlock ping-pong is the worst case (an SIO access every three instructions), so ordinary code sits between F's figure here and C's. **A catch-up is only needed for the accesses that synchronise** (the FIFO, the spinlocks, and the event instructions); the SIO's GPIO and divider registers need none, and with core 1 asleep the check is one compare.

Still not measured: `SEV` / `WFE` through the same hook; the product's loop with PIO and DMA; a cross-core IRQ; wasm; shared-RAM polling without the SIO (F has C's delay there); the re-entrancy of the nested run when a debug hook fires or a core raises an IRQ on the other inside the access.

**The third round: accuracy is a requirement, so it is tested (W5).** The maintainer's word (2026-10-18): this emulator is accurate by nature, so a scheduler that is not accurate is a problem, not a trade. The first two rounds measured cost and the SIO; they did not ask what a core sees of the *clock* inside a quantum, where the clock is not ticked per instruction. W5: core 0 reads the TIMER's microsecond counter in a loop (200 000 readings), core 1 runs the ALU loop, and the sequence of readings must equal the exact scheduler A's, reading for reading. (A first version of this test was empty - the TIMER had not been started, every reading was 0 and every candidate "matched"; found by printing the first and last reading, fixed by enabling the crystal, the reference clock and the watchdog tick the way pico-sdk does. The numbers below are from the fixed test.)

| Candidate | readings that differ from A (of 200 000) | Minstr/s (g++) |
|---|---|---|
| C quantum 16 / 256 | 9 766 (5 %) / 107 547 (54 %) | 76.7 / 85.8 |
| F quantum 16 / 256 / 1024 (the SIO ordered, the clock not) | 9 766 / 107 547 / **175 000 (87.5 %)** | 76.4 / 85.1 / 86.8 |
| **F2** quantum 16 / 256 / 1024: **every device access** orders the cores by time *and* brings the clock to the accessing core's time | **0 / 0 / 0** | 63.3 / 69.4 / 69.4 |

So F, as written above, is **not accurate for anything that reads time**: inside a quantum the clock stands still, and a core reads a counter that is up to a quantum old. F2 repairs it with the same mechanism: an access that leaves RAM and flash is a **synchronisation point** (the core that is behind catches up, then the clock is ticked to this core's time, then the access happens), and the readings are then identical to the exact scheduler's. The hook sits where the bus already leaves its fast path (the SIO slot, the PPB slot and the window table are all reached after the memory-map reads have failed), so RAM and flash accesses pay nothing; the cost is proportional to how often the firmware touches a device - the TIMER loop above is a device access every six cycles and still runs at 63-69 against A's 50.

**The accuracy contract this gives (to be agreed, then tested):**
1. *Exact* - identical to the exact scheduler A, access for access: every device access (the SIO, the PPB, every peripheral window), the event instructions (`SEV` / `WFE`, same hook), the alarms and the interrupts.
2. *Not exact, and bounded:* data that two cores exchange through **RAM or flash with no SIO or device access in between** (a flag polled in RAM) is seen by the other core up to one quantum late. The quantum is a setting (16 cycles is 128 ns; the cost of a smaller one is in the tables), and the exact scheduler A is always available as the strict mode. Correct firmware does not depend on it (a race is a race on silicon too), but a timing measurement of such a race would.
3. *Instruction-level bounds that hold for every candidate, A included* (the time architecture, [0105](0105-time-architecture-audit.md) findings 3-5): a device access happens at the instruction's start while its alarms fire at its end (up to the instruction's length, 1-3 cycles, more for `LDM`/`PUSH`/`POP`); a core waking from WFI starts at the alarm's tick, not on its own clock edge (under one cycle); a PIO pin change is seen at the end of the instruction that stepped the PIO. These do not distinguish A from F2 and are not part of what F2 gives up.
4. *Not modelled by any candidate, as in the single-core emulator today:* bus contention between the cores and the DMA (the datasheet: a main-bus access "may have to wait longer due to contention from other system bus masters"; 0098 lists it as missing).

**The test that makes it a requirement (an acceptance criterion for P1):** record, for the same firmware, the sequence of `(time, core, address, value)` over every device access under A and under the chosen scheduler, and require them **equal**; run it on MicroPython's `_thread` tests, CoreMark and the hand-written two-core programs, and add a deliberately racy program to show where (2) applies.

**Reading so far (a recommendation, not a decision):** **F2** - a quantum bounded by the next alarm, in which every access that leaves RAM and flash (the SIO, the PPB, every peripheral) and the event instructions are synchronisation points ordered by time through a nested catch-up and a tick of the clock to the accessing core's time - is the best path on the evidence: the same device-access sequence as the exact scheduler, SIO and TIMER tests both identical, at a cost between A's and C's that does not depend on the quantum. A (exact, per instruction) stays as the strict mode, for the oracle and for timing studies; the Python reference can implement either (F2 is a loop of N instructions and one sync call). B, A', C alone, E and F without the clock sync are out. Before deciding: `SEV` / `WFE` through the hook, the product's loop with PIO / DMA, an IRQ crossing cores, wasm, and the re-entrancy review of the nested run (a debug hook or an interrupt raised inside an access).

## Phases

- **P0 - the seams, no change of behaviour.** `Bus::initiator`, `Mcu::cpu1` (present, inactive), `ppb_[2]`, SIO per-core arrays, the per-core IRQ entry, `SEV` over the present cores, the core argument in the C ABI accessors. Every existing test passes unchanged. **Acceptance (all four):** (a) the callgrind instruction count of `run_batch` over the synthetic program of `scripts/bench/synthetic.py` is **identical** to the build before P0 (it is deterministic: the count, not a timing, is the proof of "nothing added"); (b) the synthetic benchmark of `scripts/bench/wasm_core.py` is within the noise (+-2 % best of 5) on the Cython build, the C++ build and the wasm module; (c) the CoreMark demo's wall time is within +-3 % (47 s for 12.07 simulated seconds on the 2.8 GHz Xeon of the sandbox of 2026-10-18); (d) the wasm module's size is reported (187 KB before).
- **P0.5 - the scheduling prototype and the decision.** The throwaway prototype of the candidates above (`scripts/bench/two_core/`), the numbers in this record - two rounds done, F leads - then D6 decided by the maintainer. No product code.
- **P1 - core 1 in C++** (after P0.5). the dual loop (the candidate chosen in P0.5), the FIFOs, the launch handshake (D7), `PSM` / `RESETS` `PROC1`, IO_BANK0 `PROC1`, `PROC1_NMI_MASK`. Tests in the bare-core harness with small hand-assembled two-core programs: FIFO ping-pong, spinlock contention, `SEV` / `WFE` wake, an IRQ on each core, the launch and the reset of core 1. **Acceptance:** P0's four, unchanged, with core 1 never launched; and the cost with two busy cores measured, expected near 2x the single-core wall time per simulated second (two cores execute twice the instructions).
- **P2 - the reference, the oracle and the shells.** The dual loop in Python, the lockstep oracle, the Cython shells (`cores`, `core1`), the `Simulator`, the GDB stub's core selection.
- **P3 - real firmware.** MicroPython `_thread`, pico-sdk's multicore examples (the Pico SDK workflow), CoreMark 1.0.4 or later (the demo can move from the pinned v1.0.0; **the README is not touched until the maintainer says**).

## How to check it: the official MicroPython suite

The maintainer's suggestion of 2026-10-18: extend the coverage of MicroPython's own test suite, in CI or locally. It already runs in CI as groups of `tests/micropython_official/run_suite.sh` in `.github/workflows/ci-micropython-official.yml` (`ports/rp2` and `extmod/machine_*`, `micropython/`, `extmod/`, `basics/`, each report-only or `STRICT`). The addition is a group **`thread/*.py`** (MicroPython's `tests/thread`), run **report-only first** - today they cannot pass, and the baseline (which hang, which fail, which skip) is worth recording *before* P1 so that P1's effect is a measured difference - and promoted to `STRICT` for the files that must pass once core 1 works (the ones that need real parallelism or timing the emulator does not give go to `EXCLUDE`, with the reason). Also candidates: `ports/rp2` tests that touch `_thread` or the FIFO. The baseline run is a small local step and does not need core 1.

## Risks and open questions

1. **Resolved by the datasheet** (2.3.2, see "Checked against the datasheet"): only the two GPIO lines are per core.
2. **Time with two cores in the PIO and the DMA** (D6): the reference steps them by "the instruction's cycles"; stepping them by the clock's delta must give identical results in the single-core case - a test for P0/P1 before the dual loop is trusted.
3. **Idle with one core waiting** and the other running (the existing idle jump is for one core).
4. **The debug hooks** (`on_break`, `log`, `bl_taken`, the GDB stub): which core.
5. **The wasm module's size** with the dual loop and core 1's state (D11).
6. **The speed of two busy cores** is not yet measured; 0096 has only the 1.34x real time for one busy core. The backends' figures of 0103 are to be re-measured after P1.
7. **Whether the real boot ROM should run on core 1** (exact) instead of the handshake emulation (D7) - not needed for the first release of core 1.
8. **Determinism** is kept (one thread, ties to core 0); free-threaded builds are unaffected (no thread of ours).

## Progress log

- 2026-10-18 (decided): the maintainer accepted the accuracy contract and chose **F2 as the main scheduler, A as the strict mode** (D6). Recorded under "What is decided"; the instruction-level bounds of 0105 added to the contract as item 3. Nothing built; P0 and everything after it still wait for an explicit go-ahead.

- 2026-10-18 (accuracy, on the maintainer's word): a third round with a **test of the clock inside a quantum** (W5) found that F is *not* accurate for anything that reads time (87 % of TIMER readings wrong at a 1024-cycle quantum); **F2** (every device access also brings the clock to the core's time) matches A reading for reading. The accuracy contract and the device-access-trace test are written above.
- 2026-10-18 (later still): **the second prototype round, on the SIO spinlocks:** E (a quantum cut after an SIO access) is not exact; **F (a quantum with every SIO access ordered by time through a nested catch-up) reproduces A to the instruction at any quantum, 1.4-1.6x faster.**
- 2026-10-18 (later): **checked against the datasheet** (it corrected the IRQ routing - only the two GPIO lines are per core - the `SEV` rule - cross-wired - and the launch - an echoing handshake, D7) and **the first prototype round** (the table above; E proposed, not yet measured).
- 2026-10-18: written (documentation only): the change of order, the audit of the tree, proposals D1-D11, the phases and their acceptance, the check through MicroPython's own suite. **Corrected the same day on the maintainer's word:** the scheduling of the two cores had been written as decided (local-time, one after the other); it is not - only "no OS thread" is. It is now an open question with candidates and a measurement plan (P0.5).
