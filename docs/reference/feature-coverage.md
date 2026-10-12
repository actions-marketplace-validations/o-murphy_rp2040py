<!-- Reference (living table). Unlike docs/records/, which are append-only, THIS FILE IS UPDATED IN PLACE: whoever changes what the chip implements updates the row in the same commit. -->

# What the emulated RP2040 implements, and what it does not

One place to see, block by block and datasheet section by section, what is modelled, what is only a stub, what is missing, and **when** each missing thing is to be done. The *why* and the evidence live in the records (every row names one); this file is the table that tells you where to work next. Every register of the datasheet, with the same kind of status, is in [register-coverage.md](register-coverage.md) (generated).

**Rules of upkeep.** (1) A change that adds, fixes or removes behaviour of a block updates its rows here in the same commit. (2) A missing feature is listed here **and** in 0098's backlog table; it is implemented only on an explicit go-ahead (CLAUDE.md, "Document vs. implement"). (3) A row says *what* is missing in the datasheet's words, never "TODO". (4) A status is never rounded up: if only part of a section works, it is Partial, and the note says which part.

**Status words.** *Done*: modelled and tested (for the audited blocks, against the datasheet). *Partial*: some of it works; the note says what does not. *Stub*: the registers exist and read back, nothing acts on them. *Missing*: not modelled. *Unchecked*: not checked when this was written - check it and fix the row. *n/a*: nothing to emulate (electrical, mechanical, an off-chip tool). **When**: *Next* = the agreed order (records 0096 / 0098); *With X* = done together with another item; *Go-ahead* = a backlog feature that waits for the maintainer's explicit word; *Needs X* = blocked by a prerequisite.

## 1. The blocks at a glance

| Block | Model | Read against the datasheet | C++ core | Main gaps |
|---|---|---|---|---|
| Cortex-M0+ CPU, NVIC, SysTick, PPB | Partial | n/a (ARMv6-M tests; MicroPython interpreter tests) | Ported | `AIRCR`, `SCR`, `CCR`, `SHCSR` and the MPU are absent; core 1 |
| SIO | Partial | Yes | Ported | inter-core FIFO; CPUID of core 1 |
| TIMER | Done | Yes | Ported | - |
| WATCHDOG | Done | Yes | Ported | - |
| RTC | Partial | Yes | Ported | clk_rtc synchroniser latency, dormant wake, GPIN reference |
| UART0/1 | Partial | Yes + second pass | Ported | receive timing, `UARTRTINTR`, per-entry error flags, flow control, modem lines |
| SPI0/1 | Partial | Yes + second pass | Ported | transmitter time, slave mode, TI/Microwire, receive timeout |
| I2C0/1 | Partial | Yes + second pass | Ported | slave mode, DREQs, `IC_SAR` and friends are stubs |
| ADC | Partial | Yes + second pass | Ported | `TS_EN` gating, SAR error flag |
| PWM | Done | Yes | Ported | - |
| DMA | Partial | Yes + second pass + the datasheet examples | Ported | sniffer, priority/arbitration, error flags, I2C/SSI DREQs, bus-cycle timing |
| PIO0/1 | Partial | Yes + second pass | Ported | FJOIN, `OUT_STICKY`/`INLINE_OUT_EN`/`OUT_EN_SEL`, autopull timing |
| SSI (XIP flash controller) | Partial | Yes + second pass | Ported | a flash-command model: no own FIFOs, interrupts, `TMOD`; 15 of 28 registers are stubs |
| CLOCKS | Partial | Yes + second pass | Python only | resus, clock enables / sleep, GPIN; 10 registers are stubs |
| PLL | Partial | Yes | Python only | always locked |
| XOSC | Partial | Yes | Ported | DORMANT is stored |
| ROSC | Missing | No | No | the whole block - **Next** |
| PSM, RESETS, SYSINFO, TBMAN | Done | Yes | Ported | - |
| SYSCFG | Stub | Yes | Ported | only `PROC0_NMI_MASK` acts |
| VREG_AND_CHIP_RESET | Partial | Yes | Ported | `VREG`, `BOD` are stored; `CHIP_RESET` acts |
| BUSCTRL | Stub | Yes | Ported | no arbitration, no performance counters |
| IO_BANK0 | Partial | No | Python only | `PROC1_*` and `DORMANT_WAKE_*` absent; not audited |
| PADS_BANK0 / PADS_QSPI / IO_QSPI | Unchecked | No | Python only | not audited; `PADS_BANK0.SWCLK/SWD` and all of IO_QSPI absent |
| XIP_CTRL | Stub | No | Python only | no cache, no streaming (record 0052); **Next** after IO_BANK0 |
| USB controller + CDC host | Partial | No | Python only | host mode works; `USB_MUXING`, `USBPHY_*` absent; device mode unchecked |

## 2. Every section of the datasheet

Follows the datasheet's own table of contents (chapter.section), so a row is something the datasheet describes. Evidence for a row is the record named in the note.

### Chapter 2 - System description

| Section | Feature | Status | Notes | When |
|---|---|---|---|---|
| 2.1.1 | AHB-Lite crossbar | Partial | no contention and no priority; `BUSCTRL.BUS_PRIORITY` is stored | Go-ahead |
| 2.1.2 | Atomic register access (XOR / SET / CLR aliases) | Done | 0098 | - |
| 2.1.3 | APB bridge | Done | | - |
| 2.1.4 | Narrow IO register writes | Unchecked | | - |
| 2.2 | Address map | Done | memory_map, the window table | - |
| 2.3.1 | SIO: divider, interpolators, spinlocks, GPIO ops | Done | 0098 | - |
| 2.3.1 | SIO: inter-core FIFO | Missing | needs core 1 (record 0053) | Needs core 1 |
| 2.3.2 | Interrupts: every IRQ line | Partial | to core 0 only | Needs core 1 |
| 2.3.3 | Event signals (SEV / WFE) | Done | | - |
| 2.3.4 | Debug | Partial | a GDB stub, not an SWD/DAP model | - |
| 2.4.1-2.4.3 | Cortex-M0+ core and programmer's model | Done | MicroPython interpreter tests: 664 files, no failure | - |
| 2.4.4 | System control | Partial | `CPUID`, `ICSR`, `VTOR` work; **`AIRCR`, `SCR`, `CCR`, `SHCSR` absent** (`SYSRESETREQ` through `AIRCR` is how `NVIC_SystemReset()` resets) | Go-ahead |
| 2.4.5 | NVIC, SysTick | Done | | - |
| 2.4.6 | MPU | Missing | `MPU_TYPE`, `MPU_CTRL`, `MPU_RNR`, `MPU_RBAR`, `MPU_RASR` absent | Go-ahead |
| 2.5.1 | DMA: configuring channels | Done | 0098 | - |
| 2.5.2 | DMA: starting (aliases, triggers, chaining, null triggers) | Done | the datasheet's two example use cases run as tests (0098) | - |
| 2.5.3 | DMA: DREQ, system table | Partial | I2C and SSI have no DREQ output | Go-ahead |
| 2.5.3.2 | DMA: credit-based DREQ scheme | Unchecked | | - |
| 2.5.4 | DMA: interrupts | Done | | - |
| 2.5.5.1 | DMA: pacing timers | Done | | - |
| 2.5.5.2 | DMA: CRC sniffer | Missing | `SNIFF_CTRL`, `SNIFF_DATA`; the algorithm's bit order needs a hardware-sourced vector | Go-ahead |
| 2.5.5.3 | DMA: channel abort | Done | | - |
| 2.5.5.4 | DMA: debug (`DBG_CTDREQ`) | Missing | | Go-ahead |
| 2.5 | DMA: `HIGH_PRIORITY` and arbitration, read/write/AHB error flags, bus-cycle timing | Missing | a transfer takes one alarm; MicroPython `rp2_dma` expects 30-80 ms for a large copy and sees 1 ms | Go-ahead |
| 2.6.1 | ROM | Done | the real boot ROM image | - |
| 2.6.2 | SRAM | Unchecked | striped / non-striped, per-bank | - |
| 2.6.3 | Flash and the XIP cache | Stub | XIP_CTRL registers exist, no cache, no streaming | With the XIP_CTRL port |
| 2.7 | Boot sequence | Done | flash boot with boot2: MicroPython, CircuitPython, Kaluma, SDK examples in CI | - |
| 2.8.1, 2.8.3 | Bootrom: processor-controlled boot, ROM contents | Done | flash routines go through the SSI | - |
| 2.8.2 | Bootrom: launching code on core 1 | Missing | | Needs core 1 |
| 2.8.4, 2.8.5 | Bootrom: USB mass storage, PICOBOOT | Unchecked | | - |
| 2.9, 2.10 | Power supplies, core regulator (analogue) | n/a | the `VREG` / `BOD` registers are stored (Stub) | - |
| 2.11.1 | Top-level clock gates | Missing | | Go-ahead |
| 2.11.2 | SLEEP state | Unchecked | | - |
| 2.11.3 | DORMANT state | Missing | `XOSC.DORMANT` is stored; `ROSC` is missing | Go-ahead |
| 2.11.4 | Memory power down | Stub | `SYSCFG.MEMPOWERDOWN` is stored | - |
| 2.12 | Chip-level reset: power-on, external, watchdog, source of last reset | Done | 0098 | - |
| 2.12.3 | Brown-out detection | Stub | `BOD` is stored | - |
| 2.12.6 | Rescue debug port reset | Missing | | - |
| 2.13 | Power-on state machine (incl. the watchdog interaction) | Done | 0098 | - |
| 2.14 | Subsystem resets | Done | 0098 | - |
| 2.15.1-2.15.3 | Clocks: overview, multiplexers, dividers, generators | Done | | - |
| 2.15.2 | Clock sources: XOSC, PLLs | Done | PLL always locked | - |
| 2.15.2.1 | Clock source: ROSC | Missing | | **Next** |
| 2.15.2.3 | Clock source: external clocks (GPIN) | Missing | | Go-ahead |
| 2.15.3.4 | Duty cycle correction | Unchecked | | - |
| 2.15.3.5 | Clock enables (`WAKE_EN` / `SLEEP_EN` / `ENABLED0/1`), system sleep | Stub | stored; `ENABLED0/1` absent; needs the cores' sleep state | Go-ahead |
| 2.15.4 | Frequency counter `FC0_*` | Done | 0098 | - |
| 2.15.5 | Resus | Missing | the `FRCE` slice needs no stopped-clock model; the detection does | Go-ahead |
| 2.15.6 | Programmer's model: configuring a generator | Done | | - |
| 2.15.6.3 | A GPIO output clock | Unchecked | | - |
| 2.15.6.4, 2.15.6.5, 2.15.6.6 | A GPIO input clock, enabling resus, configuring sleep mode | Missing | | Go-ahead |
| 2.16.1-2.16.3 | XOSC: overview, usage, start-up delay | Done | 0098 | - |
| 2.16.4 | XOSC counter | Unchecked | | - |
| 2.16.5 | XOSC DORMANT | Stub | stored, no effect | Go-ahead |
| 2.17 | **ROSC**: overview, trade-offs, frequency, divider, random number generator, counter, DORMANT | Missing | the whole chapter; `rosc_freq` is an attribute the clock tree reads | **Next** |
| 2.18 | PLL | Partial | always locked, no start-up time | - |
| 2.19.2 | GPIO function select | Done | | - |
| 2.19.3 | GPIO interrupts | Partial | core-0 registers work; **`PROC1_*` and `DORMANT_WAKE_*` absent** (24 register tables); IO_BANK0 never audited | With the IO_BANK0 port |
| 2.19.4 | Pads | Partial | `PADS_BANK0.SWCLK` / `SWD` absent; never audited | With the IO_BANK0 port |
| 2.20-2.22 | Sysinfo, Syscfg, TBMAN | Done | Syscfg is mostly stored (Stub) | - |

### Chapter 3 - PIO

| Section | Feature | Status | Notes | When |
|---|---|---|---|---|
| 3.2 | Programmer's model (programs, control flow, registers, stalling, pin mapping, IRQ flags, interactions) | Done | | - |
| 3.3 | PIO assembler (`pioasm`) | n/a | an off-chip tool; the emulator runs assembled programs | - |
| 3.4 | Instruction set (JMP, WAIT, IN, OUT, PUSH, PULL, MOV, IRQ, SET) | Done | | - |
| 3.5.1, 3.5.2 | Side-set, program wrapping | Done | | - |
| 3.5.3 | FIFO joining (`FJOIN_TX/RX`) | Missing | MicroPython `PIO.JOIN_TX` / `JOIN_RX`; the bits are stored and ignored | Go-ahead |
| 3.5.4 | Autopush and autopull | Partial | the autopull happens when the next `OUT` starts, not at the `OUT` that reaches the threshold | Go-ahead |
| 3.5.5 | Clock dividers | Done | | - |
| 3.5.6 | GPIO mapping | Partial | `OUT_STICKY`, `INLINE_OUT_EN`, `OUT_EN_SEL` and the input synchroniser bypass are stored and ignored | Go-ahead |
| 3.5.7 | Forced and EXEC'd instructions | Done | | - |
| 3.6 | Examples | Partial | WS2812 is a test; duplex SPI, UART TX/RX, Manchester, BMC, I2C, PWM, addition are Unchecked | - |

### Chapter 4 - Peripherals

| Section | Feature | Status | Notes | When |
|---|---|---|---|---|
| 4.1 | USB controller as a host with the Python USB device and the CDC console | Done | not audited against the datasheet | With the USB port |
| 4.1 | USB device mode | Unchecked | | - |
| 4.1.3 | `USB_MUXING`, `USBPHY_DIRECT*`, `USBPHY_TRIM` | Missing | | With the USB port |
| 4.2.2, 4.2.3 | UART: functional description, operation, transmitter time | Done | 0098 | - |
| 4.2.3 | UART: receive timing | Missing | bytes fed from outside arrive at once | Go-ahead |
| 4.2.4 | UART: hardware flow control (CTS / RTS) | Missing | | Go-ahead |
| 4.2.5 | UART: DMA interface | Partial | `UARTTXDMASREQ` / `UARTRXDMASREQ`; the burst requests are not modelled | - |
| 4.2.6 | UART: interrupts | Partial | RX, TX, overrun work; `UARTRTINTR` (MicroPython `IRQ_RXIDLE`), modem, BE/PE/FE are never raised | Go-ahead |
| 4.2.7 | UART: per-entry error flags `DR[11:8]` | Missing | | Go-ahead |
| 4.3.5-4.3.7 | I2C: behaviour, protocols, TX FIFO management with START / STOP / RESTART | Done | master mode, 0098 | - |
| 4.3.8 | I2C: multiple master arbitration | Done | `ARB_LOST` | - |
| 4.3.9 | I2C: clock synchronisation | Unchecked | | - |
| 4.3.10.1 | I2C: slave mode | Missing | needs an external master on the bus (requirements in 0098) | Needs a bus model |
| 4.3.10.2-4.3.10.4 | I2C: master mode, disabling, aborting | Done | | - |
| 4.3.11 | I2C: spike suppression | Stub | registers only | - |
| 4.3.12, 4.3.14 | I2C: fast mode plus, IC_CLK configuration | Done | registers | - |
| 4.3.13 | I2C: bus clear | n/a | the chip has no registers for it | - |
| 4.3.15 | I2C: DMA interface | Stub | registers stored; **no DREQ** | Go-ahead |
| 4.3.16 | I2C: interrupt registers | Done | | - |
| 4.4 | SPI (PL022): Motorola SPI frame format, master, FIFOs, DMA requests | Done | 0098 | - |
| 4.4 | SPI: transmitter time | Missing | MicroPython `machine_spi_rate` fails for it | Go-ahead |
| 4.4 | SPI: slave mode, receive timeout, TI and Microwire frames | Missing | | Go-ahead |
| 4.5 | PWM: counter, TOP, compare A/B, phase-correct, dividers, B-pin counting modes, `PH_ADV` / `PH_RET`, wrap interrupt | Done | 0098 | - |
| 4.6 | Timer: 64-bit counter, `TIMEHW` / `TIMELW`, four alarms, `PAUSE`, `DBGPAUSE` | Done | 0098 | - |
| 4.7 | Watchdog: tick generation, counter, scratch registers, `REASON`, reboot | Done | the tick is also the TIMER's and SysTick's reference | - |
| 4.8 | RTC: storage format, leap years, interrupts, reference clock | Done | 0098 | - |
| 4.8.4 | RTC: clk_rtc synchroniser latency | Missing | `RTC_ACTIVE`, `MATCH_ACTIVE`, LOAD arrive after 2 clk_rtc periods | Go-ahead |
| 4.9.1, 4.9.2 | ADC: controller, SAR ADC (one-shot, free-running, round-robin, FIFO, DMA, interrupts) | Done | 0098 | - |
| 4.9.5 | ADC: temperature sensor as channel 4 | Partial | `CS.TS_EN` does not gate it | Go-ahead |
| 4.9.3, 4.9.4 | ADC: ENOB, INL / DNL | n/a | silicon characteristics | - |
| 4.10 | SSI: the flash-command model used by the boot ROM and XIP, `DR` aliases, clear-on-disable | Done | 0098 | - |
| 4.10.5, 4.10.7, 4.10.8 | SSI: own 16-entry FIFOs, the five interrupts, `TMOD` 00/01/10/11 | Stub | 15 of 28 registers are stubs; requirements in 0098 | Go-ahead |
| 4.10.6 | SSI: 32-bit frame size | Missing | | Go-ahead |
| 4.10.11 | SSI: DMA interface | Missing | | Go-ahead |
| 4.10 | SSI: XIP streaming | Missing | read datasheet 2.6 first | With the XIP_CTRL port |

### Chapters 1 and 5

Introduction (1), electrical and mechanical (5): n/a.

## 3. Upstream conformance suites

| Suite | What it checks | Where | State |
|---|---|---|---|
| MicroPython `tests/` against the emulated board (CI: *MicroPython official tests*, four parallel report-only groups; `tests/micropython_official/run_suite.sh`) | peripherals (`ports/rp2`, `extmod/machine_*`) and the interpreter (`float`, `basics`, `inlineasm/thumb`) | 0098, "MicroPython's own test suite" and "The interpreter tests" | interpreter: 0 failures; peripherals: the DMA copy time and the UART timing tests (UART now passes), `machine_spi_rate` (not diagnosed; probably the missing transmitter time), and core 1 (`_thread` files are skipped) |
| MicroPython `tests/` not yet run | `extmod` beyond `machine_*`, `micropython`, `import`, `misc`, `io`, `stress`, `cmdline`, `thread` (needs core 1) | - | Missing: scan locally, then add as a group (Go-ahead) |
| pico-sdk / pico-examples | firmware boots and prints (`hello_usb`), eight SDK versions | CI: *Test Pi Pico SDK* | passing |
| CircuitPython, Kaluma | boot and run a script, several releases | CI | passing |

## 4. Cross-cutting

| Item | Status | Notes | When |
|---|---|---|---|
| WASM host and the drop-in for rp2040js | Missing | record 0099, step zero not started | Go-ahead |
| Consolidating the Cython shells | Missing | less wrapper code once the ports are done | After the 0096 gate |
| An intermediate single-core C++ release | Missing | after the gate review; keeps the three dual-core seams from 0096 (initiator id on bus accesses, IRQ line to core mask, per-core CPU state) | After the 0096 gate |
