# 0098 - the ported blocks against the RP2040 datasheet

Status: in progress (opened 2026-10-06). Companion to [0096](0096-cpp-mcu-core.md): the C++ ports are held to the pure-Python *reference* by lockstep oracles, which proves the C++ is the
reference - not that the reference is the chip. The references came from rp2040js and from reading, in sessions that could not reach the manuals; the egress was opened for
`*.arm.com` and `*.raspberrypi.com` on 2026-10-06, so this record is where every block that is ported on the 0096 branch is read against the datasheet (`RP-008371-DS-1`).

## Method

1. **Register level, mechanical** - `scripts/audit/datasheet_conformance.py <datasheet text>`. The datasheet is turned into text with `pdftotext -layout`; the tool parses each "BLOCK: NAME Register"
   table (offset, bits, type, reset) and, on a fresh pure-Python chip, reads the register and writes it all ones and reads it back. Its categories and its blind spots are in its docstring; the short
   version: it sees resets of *writable* fields, reserved or read-only bits that stick, plain RW bits that do not, and registers the reference does not have. It does not see behaviour, the registers
   after the first of a `CH0_..., CH1_...` header, or read side effects. A finding is a question for the datasheet.
2. **Behaviour, by reading** - per block, the datasheet's description of what the block does against the reference's code. Each block gets its own section below when it is done.
3. **Fixes** - one commit per fix or per same-class group inside a block, in the reference and the C++ together (the oracles keep them equal), with an oracle mutant and a test; both builds green.

Where the datasheet is silent or defers to the ARM manual (as for SysTick in 0096), the note says so and what is the model's own.

## Register-level findings (first pass, before triage)

*Re-run after the fixes below (end of 2026-10-06, the tool's parser now also reads the per-slice and per-channel tables): 296 registers checked, 47 with findings, `{'unimplemented': 19, 'rw-not-sticking': 16, 'unimpl-wo': 3, 'reset': 5, 'raises': 1}`. What is left is the tool's own blind spots (write-only and status registers, placeholder resets, PIO CTRL needing an event loop) and the features named "left" or "not implemented" in each section: the DMA sniffer, the clock frequency counter FC0_*, CLOCKS ENABLED0/1, the inter-core FIFO, the I2C and SSI DMA/interrupt machinery. The list below is the first pass as it stood before any fix.*

Checked: 282 registers of TIMER, PWM, ADC, UART, SPI, I2C, DMA, PIO, SSI, SIO, CLOCKS, PLL. Findings by category: {'reserved-or-ro-sticks': 24, 'unimplemented': 57, 'reset': 11, 'rw-not-sticking': 20, 'unimpl-wo': 1, 'raises': 1}.
Known tool weaknesses seen in this pass: write-only registers whose table has a single unnamed field are not recognised as write-only (TIMER TIMEHW/TIMELW, UART UARTICR, SPI SSPICR show as "unimplemented"
though nothing reads them on the chip); the PCELLID/PERIPHID rows are read-only IDs the tool judges as writable; PIO CTRL needs an event loop, which the tool does not provide.


### TIMER  (17 registers, 3 with findings)

- `TIMER.TIMEHW` 0x00
  - unimplemented (read warns)
- `TIMER.TIMELW` 0x04
  - unimplemented (read warns)
- `TIMER.DBGPAUSE` 0x2c
  - unimplemented (read warns)

### PWM  (5 registers, 0 with findings)

No register-level findings.


### ADC  (9 registers, 4 with findings)

- `ADC.CS` 0x00
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00000100, datasheet 0x00000000
  - reserved or read-only bits changed by a write of ones: 0x00000100 (read 0x00000100 -> 0x001f700b; datasheet writable mask 0x001f740f)
- `ADC.FCS` 0x08
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00000100, datasheet 0x00000000
- `ADC.DIV` 0x10
  - reserved or read-only bits changed by a write of ones: 0xff000000 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x00ffffff)
- `ADC.INTR` 0x14
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00000001, datasheet 0x00000000

### UART  (21 registers, 8 with findings)

- `UART.UARTRSR` 0x004
  - unimplemented (read warns)
- `UART.UARTILPR` 0x020
  - unimplemented (read warns)
- `UART.UARTLCR_H` 0x02c
  - reserved or read-only bits changed by a write of ones: 0xffffff00 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x000000ff)
- `UART.UARTCR` 0x030
  - reserved or read-only bits changed by a write of ones: 0xffff0078 (read 0x00000300 -> 0xffffffff; datasheet writable mask 0x0000ff87)
- `UART.UARTIFLS` 0x034
  - unimplemented (read warns)
- `UART.UARTICR` 0x044
  - unimplemented (read warns)
- `UART.UARTDMACR` 0x048
  - unimplemented (read warns)
- `UART.UARTPCELLID3` 0xffc
  - reset: read 0x000000b1, datasheet 0x00000065, differing writable bits 0x000000d4
  - RW bits that read 0 after a write of ones: 0x0000034e (read back 0x000000b1)

### SPI  (18 registers, 8 with findings)

- `SPI.SSPCR0` 0x000
  - reserved or read-only bits changed by a write of ones: 0xffff0000 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x0000ffff)
- `SPI.SSPCR1` 0x004
  - reserved or read-only bits changed by a write of ones: 0xfffffff0 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x0000000f)
- `SPI.SSPCPSR` 0x010
  - RW bits that read 0 after a write of ones: 0x00000001 (read back 0x000000fe)
- `SPI.SSPIMSC` 0x014
  - reserved or read-only bits changed by a write of ones: 0xfffffff0 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x0000000f)
- `SPI.SSPRIS` 0x018
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00000000, datasheet 0x00000008
- `SPI.SSPICR` 0x020
  - unimplemented (read warns)
- `SPI.SSPDMACR` 0x024
  - reserved or read-only bits changed by a write of ones: 0xfffffffc (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x00000003)
- `SPI.SSPPCELLID3` 0xffc
  - reset: read 0x000000b1, datasheet 0x0000ffff, differing writable bits 0x0000ff4e
  - RW bits that read 0 after a write of ones: 0xffffff4e (read back 0x000000b1)

### I2C  (40 registers, 14 with findings)

- `I2C.IC_TAR` 0x04
  - RW bits that read 0 after a write of ones: 0x00000c00 (read back 0x000003ff)
- `I2C.IC_DATA_CMD` 0x10
  - RW bits that read 0 after a write of ones: 0x000000ff (read back 0x00000000)
- `I2C.IC_RX_TL` 0x38
  - RW bits that read 0 after a write of ones: 0x000000ef (read back 0x00000010)
- `I2C.IC_TX_TL` 0x3c
  - RW bits that read 0 after a write of ones: 0x000000ef (read back 0x00000010)
- `I2C.IC_ENABLE` 0x6c
  - reserved or read-only bits changed by a write of ones: 0xfffffff8 (read 0x00000000 -> 0xfffffffd; datasheet writable mask 0x00000007)
  - RW bits that read 0 after a write of ones: 0x00000002 (read back 0xfffffffd)
- `I2C.IC_SDA_HOLD` 0x7c
  - RW bits that read 0 after a write of ones: 0x00fffffe (read back 0x00000001)
- `I2C.IC_SLV_DATA_NACK_ONLY` 0x84
  - unimplemented (read warns)
- `I2C.IC_DMA_CR` 0x88
  - unimplemented (read warns)
- `I2C.IC_DMA_TDLR` 0x8c
  - unimplemented (read warns)
- `I2C.IC_DMA_RDLR` 0x90
  - unimplemented (read warns)
- `I2C.IC_SDA_SETUP` 0x94
  - unimplemented (read warns)
- `I2C.IC_ACK_GENERAL_CALL` 0x98
  - unimplemented (read warns)
- `I2C.IC_CLR_RESTART_DET` 0xa8
  - unimplemented (read warns)
- `I2C.IC_COMP_PARAM_1` 0xf4
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00000000, datasheet 0x3230312a

### DMA  (13 registers, 7 with findings)

- `DMA.INTS1` 0x41c
  - RW bits that read 0 after a write of ones: 0xffffffff (read back 0x00000000)
- `DMA.MULTI_CHAN_TRIGGER` 0x430
  - unimplemented (read warns)
- `DMA.SNIFF_CTRL` 0x434
  - unimplemented (read warns)
- `DMA.SNIFF_DATA` 0x438
  - unimplemented (read warns)
- `DMA.FIFO_LEVELS` 0x440
  - unimplemented (read warns)
- `DMA.CHAN_ABORT` 0x444
  - unimplemented (read warns)
- `DMA.N_CHANNELS` 0x448
  - reset: read 0x0000000c, datasheet 0x00000000, differing writable bits 0x0000000c
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x0000000c, datasheet 0x00000000

### PIO  (17 registers, 3 with findings)

- `PIO.CTRL` 0x000
  - write/read-back raised RuntimeError: no running event loop
- `PIO.DBG_CFGINFO` 0x044
  - reset: read 0x00200404, datasheet 0x140df000, differing writable bits 0x142df404
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00200404, datasheet 0x00000000
  - RW bits that read 0 after a write of ones: 0xffdffbfb (read back 0x00200404)
- `PIO.INTR` 0x128
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x000000f0, datasheet 0x00000000

### SSI  (28 registers, 23 with findings)

- `SSI.CTRLR0` 0x00
  - reserved or read-only bits changed by a write of ones: 0xfe800000 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x017fffff)
- `SSI.CTRLR1` 0x04
  - reserved or read-only bits changed by a write of ones: 0xffff0000 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x0000ffff)
- `SSI.SSIENR` 0x08
  - reserved or read-only bits changed by a write of ones: 0xfffffffe (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x00000001)
- `SSI.MWCR` 0x0c
  - unimplemented (read warns)
- `SSI.SER` 0x10
  - unimplemented (read warns)
- `SSI.BAUDR` 0x14
  - reserved or read-only bits changed by a write of ones: 0xffff0000 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x0000ffff)
- `SSI.TXFTLR` 0x18
  - unimplemented (read warns)
- `SSI.RXFTLR` 0x1c
  - unimplemented (read warns)
- `SSI.TXFLR` 0x20
  - reserved or read-only bits changed by a write of ones: 0xffffffff (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x00000000)
- `SSI.SR` 0x28
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00000006, datasheet 0x00000000
- `SSI.IMR` 0x2c
  - unimplemented (read warns)
- `SSI.ISR` 0x30
  - unimplemented (read warns)
- `SSI.RISR` 0x34
  - unimplemented (read warns)
- `SSI.TXOICR` 0x38
  - unimplemented (read warns)
- `SSI.RXOICR` 0x3c
  - unimplemented (read warns)
- `SSI.RXUICR` 0x40
  - unimplemented (read warns)
- `SSI.MSTICR` 0x44
  - unimplemented (read warns)
- `SSI.ICR` 0x48
  - unimplemented (read warns)
- `SSI.DMACR` 0x4c
  - unimplemented (read warns)
- `SSI.DMATDLR` 0x50
  - unimplemented (read warns)
- `SSI.DMARDLR` 0x54
  - unimplemented (read warns)
- `SSI.DR0` 0x60
  - RW bits that read 0 after a write of ones: 0xffffffff (read back 0x00000000)
- `SSI.SPI_CTRLR0` 0xf4
  - reset: read 0x00000000, datasheet 0x03000000, differing writable bits 0x03000000
  - reserved or read-only bits changed by a write of ones: 0x00f804c0 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0xff07fb3f)

### SIO  (62 registers, 15 with findings)

- `SIO.GPIO_HI_IN` 0x008
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00000002, datasheet 0x00000000
- `SIO.GPIO_HI_OUT` 0x030
  - reserved or read-only bits changed by a write of ones: 0x3fffffc0 (read 0x00000000 -> 0x3fffffff; datasheet writable mask 0x0000003f)
- `SIO.GPIO_HI_OE` 0x040
  - reserved or read-only bits changed by a write of ones: 0x3fffffc0 (read 0x00000000 -> 0x3fffffff; datasheet writable mask 0x0000003f)
- `SIO.FIFO_ST` 0x050
  - unimplemented (read warns)
- `SIO.FIFO_WR` 0x054
  - unimplemented (read warns)
- `SIO.FIFO_RD` 0x058
  - unimplemented (read warns)
- `SIO.DIV_UDIVISOR` 0x064
  - reset: read 0x00000001, datasheet 0x00000000, differing writable bits 0x00000001
- `SIO.DIV_SDIVISOR` 0x06c
  - reset: read 0x00000001, datasheet 0x00000000, differing writable bits 0x00000001
- `SIO.DIV_CSR` 0x078
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00000000, datasheet 0x00000001
- `SIO.INTERP0_ACCUM0_ADD` 0x0b4
  - RW bits that read 0 after a write of ones: 0x00fffffe (read back 0x00000001)
- `SIO.INTERP0_ACCUM1_ADD` 0x0b8
  - RW bits that read 0 after a write of ones: 0x00fffffe (read back 0x00000001)
- `SIO.INTERP0_BASE_1AND0` 0x0bc
  - unimplemented (read warns) - write-only per datasheet
- `SIO.INTERP1_ACCUM0_ADD` 0x0f4
  - RW bits that read 0 after a write of ones: 0x00fffffe (read back 0x00000001)
- `SIO.INTERP1_ACCUM1_ADD` 0x0f8
  - RW bits that read 0 after a write of ones: 0x00fffffe (read back 0x00000001)
- `SIO.INTERP1_BASE_1AND0` 0x0fc
  - unimplemented (read warns)

### CLOCKS  (48 registers, 22 with findings)

- `CLOCKS.CLK_REF_DIV` 0x34
  - reset: read 0x00000000, datasheet 0x00000100, differing writable bits 0x00000100
  - reserved or read-only bits changed by a write of ones: 0x00000030 (read 0x00000000 -> 0x00000030; datasheet writable mask 0x00000300)
  - RW bits that read 0 after a write of ones: 0x00000300 (read back 0x00000030)
- `CLOCKS.CLK_USB_DIV` 0x58
  - reserved or read-only bits changed by a write of ones: 0xfffffcff (read 0x00000100 -> 0xffffffff; datasheet writable mask 0x00000300)
- `CLOCKS.CLK_ADC_DIV` 0x64
  - reset: read 0x00000000, datasheet 0x00000100, differing writable bits 0x00000100
  - reserved or read-only bits changed by a write of ones: 0x00000030 (read 0x00000000 -> 0x00000030; datasheet writable mask 0x00000300)
  - RW bits that read 0 after a write of ones: 0x00000300 (read back 0x00000030)
- `CLOCKS.CLK_RTC_DIV` 0x70
  - reset: read 0x00000000, datasheet 0x00000100, differing writable bits 0x00000100
  - RW bits that read 0 after a write of ones: 0xffffffcf (read back 0x00000030)
- `CLOCKS.CLK_SYS_RESUS_CTRL` 0x78
  - RW bits that read 0 after a write of ones: 0x00011100 (read back 0x000000ff)
- `CLOCKS.FC0_REF_KHZ` 0x80
  - unimplemented (read warns)
- `CLOCKS.FC0_MIN_KHZ` 0x84
  - unimplemented (read warns)
- `CLOCKS.FC0_MAX_KHZ` 0x88
  - unimplemented (read warns)
- `CLOCKS.FC0_DELAY` 0x8c
  - unimplemented (read warns)
- `CLOCKS.FC0_INTERVAL` 0x90
  - unimplemented (read warns)
- `CLOCKS.FC0_SRC` 0x94
  - unimplemented (read warns)
- `CLOCKS.FC0_STATUS` 0x98
  - unimplemented (read warns)
- `CLOCKS.WAKE_EN0` 0xa0
  - unimplemented (read warns)
- `CLOCKS.WAKE_EN1` 0xa4
  - unimplemented (read warns)
- `CLOCKS.SLEEP_EN0` 0xa8
  - unimplemented (read warns)
- `CLOCKS.SLEEP_EN1` 0xac
  - unimplemented (read warns)
- `CLOCKS.ENABLED0` 0xb0
  - unimplemented (read warns)
- `CLOCKS.ENABLED1` 0xb4
  - unimplemented (read warns)
- `CLOCKS.INTR` 0xb8
  - unimplemented (read warns)
- `CLOCKS.INTE` 0xbc
  - unimplemented (read warns)
- `CLOCKS.INTF` 0xc0
  - unimplemented (read warns)
- `CLOCKS.INTS` 0xc4
  - unimplemented (read warns)

### PLL  (4 registers, 4 with findings)

- `PLL.CS` 0x0
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x80000001, datasheet 0x00000000
  - reserved or read-only bits changed by a write of ones: 0x7ffffec0 (read 0x80000001 -> 0xffffffff; datasheet writable mask 0x0000013f)
- `PLL.PWR` 0x4
  - reserved or read-only bits changed by a write of ones: 0xffffffd2 (read 0x0000002d -> 0xffffffff; datasheet writable mask 0x0000002d)
- `PLL.FBDIV_INT` 0x8
  - reserved or read-only bits changed by a write of ones: 0xfffff000 (read 0x00000000 -> 0xffffffff; datasheet writable mask 0x00000fff)
- `PLL.PRIM` 0xc
  - reset: read 0x00077000, datasheet 0x0007701f, differing writable bits 0x0000001f
  - note (read-only bits, the datasheet's reset may be a placeholder): read 0x00077000, datasheet 0x00000000
  - reserved or read-only bits changed by a write of ones: 0xcff88ce0 (read 0x00077000 -> 0xffffffff; datasheet writable mask 0x3007731f)

## Triage and the behavioural audit

Per block, in the order of the 0096 ports. Each section says what the datasheet says, what the reference did, what was changed (reference and C++ together) and what is left.

### TIMER (datasheet 4.6)

Read: 4.6.1-4.6.4 and the register tables. The reference implemented the counter, the latch (`TIMELR`/`TIMEHR`), `TIMERAWL/H`, the four alarms with `ARMED`, `INTR/INTE/INTF/INTS`, and stored `PAUSE`.

- **`PAUSE` did nothing but store its bit and warn "Unimplemented Timer Pause".** The datasheet: "Set high to pause the timer". Now the count freezes, no alarm can come due, an alarm written while paused is armed and waits, and clearing `PAUSE` resumes from the frozen count with the armed alarms re-timed.
- **`TIMEHW`/`TIMELW` (write the time) were unimplemented** (a write warned and was dropped). The datasheet: "always write timelw before timehw"; "writes do not get copied to time until timehw is written". `TIMELW` is now a latch and the `TIMEHW` write sets the 64-bit time; the armed alarms are re-timed against the new count (an alarm matches the counter, not a moment of simulated time).
- **`DBGPAUSE` was unimplemented.** A 2-bit RW register (DBG1, DBG0), 1 at reset (`0x6`). Stored; no debugger is ever attached, so it changes nothing.
- **The 64-bit count was not bounded:** with the time now settable, `TIMERAWH`/`TIMEHR` could exceed 32 bits (the bus raised) and the C++ `to_uint32(double)` was undefined beyond int64. The high word is now masked to 32 bits (the counter wraps, as a 64-bit one does) and `to_uint32` is Python's `int(x) & 0xFFFFFFFF` exactly, read from the double's bits. The parity test found it the first time the new registers were in its stream.
- **Left, deliberately:** the timer counts whether or not the watchdog's 1 us tick is running (the datasheet: "The Watchdog tick must be running for the timer to start counting"). The tick belongs to the WATCHDOG, which is not ported yet; it is on that block's list. Reading `TIMEHW`/`TIMELW` (write-only) warns as an unimplemented read.
- Tests: `tests/test_timer.py` (the four behaviours), `tests/test_timer_parity.py` (the new offsets are in the random stream, 60 seeds), `tests/cpp/test_timer.cpp`.

### ADC (datasheet 4.9)

Read: 4.9.1-4.9.2.7 and the register tables.

- **The FIFO had 4 entries; the datasheet says "Eight element receive sample FIFO"** (and `FCS.LEVEL` is 4 bits wide). The reference and the C++ block now hold 8, so an overflow comes at the ninth sample and `LEVEL` reaches 8. The oracle's old mutant "the depth is 8" is now "the depth is 4".
- **`CS.READY` was 1 whenever no conversion was running, including with the ADC disabled.** The datasheet: "writing a 1 to CS.EN will start a short internal power-up sequence ... After a few clock cycles, CS.READY will go high"; reset value 0. READY is now `EN && !busy` (the power-up is not timed). pico-sdk's `adc_init` waits for it after setting EN, so a firmware that does is unaffected.
- **`DIV` stored the whole word;** bits 31:24 are reserved (INT is 23:8, FRAC 7:0). Masked to 24 bits.
- **Checked and consistent:** one conversion = 96 cycles of a 48 MHz clk_adc = 2 us; the pacing divider "once per n + 1 cycles" (`1 + INT + FRAC/256`) with the next start measured from the start of the previous one; `START_ONCE` self-clearing; the FIFO word (`ERR` bit 15, 12-bit value, `SHIFT` to bits 11:4); `OVER`/`UNDER` write-1-to-clear; the threshold for IRQ and DREQ is `level >= THRESH`; the round-robin order; `INTS`.
- **Left, deliberately:** the clk_adc frequency is a fixed 48 MHz (the datasheet: the clock "must be set up correctly before enabling the ADC"; a model that followed `CLK_ADC` would never convert for a firmware that does not program it, which the tests do not); `DIV.FRAC` is an average, not the first-order delta-sigma; "the divider is reset when either of these fields are written" is not modelled; `INTR` is 1 at `THRESH` 0 because `level >= 0` always holds (the datasheet's reset value is 0 - unresolved: it does not say what the comparator does at a threshold of 0).
- Tests: `tests/test_adc.py` (FIFO depth, READY, DIV mask), `tests/cpp/test_adc.cpp`, and the oracle's mutants (`fifo_depth_4`, `cs_ready_without_en`, `div_unmasked`; two of the old ones turned over).

#### UART transmitter time (2026-10-12): the model that the second pass and MicroPython's own tests asked for

The UART second pass listed the TX-interrupt rule and the missing time base as gaps; MicroPython's own `machine_uart_tx.py` and `machine_uart_irq_txidle.py` (run through the pty bridge, see the section on MicroPython's test suite) failed for the same root. The transmitter now has a time, in the reference and the C++ together:

- **A TX FIFO and a shift register.** A `UARTDR` write enters a 32-entry FIFO (a one-entry holding register without `LCR_H.FEN`; a write to a full FIFO is lost, as on the chip). The shift register takes one character at a time and holds it for `frame bits x bit time`: start + `WLEN` data + parity (`PEN`) + 1 or 2 stop bits (`STP2`), the bit time being `16 x divider / clk_peri`. Only when the frame has gone out is the character delivered (`on_byte`, or the receiver under `LBE`). The shift register is an alarm of the chip's clock, so the native UART now needs the native `SimulationClock` like the ADC.
- **`UARTFR`:** `TXFE` (FIFO empty - the character in the shift register does not count), `TXFF` (full), `BUSY` (shifting or FIFO not empty: "set as soon as the transmit FIFO becomes non-empty ... until the complete byte, including all the stop bits, has been sent").
- **`UARTTXINTR` is a transition** (4.2.6.3): raised when a character leaving the FIFO takes it from above the `UARTIFLS.TXIFLSEL` level (4/8/16/24/28 of 32; 0 without FEN) to at or below it; cleared by `UARTICR` or by writes taking the FIFO above the level; **not raised again by the characters that follow**. The first version raised it on every character at or below the level and MicroPython saw five `IRQ_TXIDLE` for one message: pico-sdk's `uart_set_irq_enables` writes TXIFLSEL 0 ("minimum threshold"), so the one interrupt belongs to the 5-to-4 transition. `IFLS` is therefore no longer "stored, not acted on" for its TX field.
- **The TX DMA request** is `UARTTXDMASREQ`: "at least one empty location in the transmit FIFO" (UARTEN, TXE, `TXDMAE`, not `TXFF`), re-announced as the FIFO fills and drains and when `FEN` changes its depth.
- **Disabling mid-frame** lets the character on the wire finish; the rest waits, and re-enabling resumes it. **Reset** drops the FIFO, the shift register and the alarm.
- **With a zero divider (no baud rate configured) the bit time is 0 and a character leaves at once, as it always did** - every test and tool that never set a baud rate is unchanged (`FR` shows `TXFE`, `TXINTR` follows each write when the FIFOs are off).
- **Results:** MicroPython's `machine_uart_tx.py` and `machine_uart_irq_txidle.py` pass (they failed before). Tests: `tests/test_uart.py` (five datasheet tests on both builds), `tests/cpp/test_uart.cpp` (the timed transmitter, trigger levels, failure injection; the textual mutation pass over the TX code leaves only four equivalent survivors - the redundant `moved = false` initialisations), the oracle's `generate_tx` microscope stream and 55 new logic mutants (frame length, FIFO depth, trigger table, the transition rule and its off-by-ones, BUSY/TXFF/TXFE, the DMA request, delivery at the start of the frame, reset leaving the FIFO / shift register / alarm).
- **Still not modelled:** `UARTRTINTR` (the receive timeout), the per-entry `DR[11:8]` error flags, break/parity/framing errors, hardware flow control (`CTS`/`RTS`), the receive side's timing (bytes fed from outside still arrive instantly).

#### ADC, second pass (2026-10-12): the operation chapter 4.9.2

Read 4.9.1-4.9.5 against `_adc.py` and `core/adc.hpp`. Two nonconformances, fixed in the reference and the C++ together:

- **A `FIFO` read did not re-evaluate the interrupt line.** The datasheet (4.9.2.6): "The interrupt is cleared by draining the FIFO to a level lower than FCS.THRESH". `INTR`/`INTS` (derived) fell, but the NVIC line stayed up until some later event, so a handler that drained the FIFO and returned was entered again. The C++ test even pinned the old behaviour ("a FIFO read does not touch the line"); it now pins the datasheet's.
- **Clearing `CS.EN` did not stop a free-running capture.** The datasheet (4.9.2): "The ADC can be disabled again at any time by clearing CS.EN". A start needed EN, but the restart after each conversion and the pacing-gap alarm only looked at START_MANY. Both now need EN as well (the conversion in flight still completes).
- **Checked and consistent:** one-shot start and READY, `AINSEL` any time before the start, round-robin order (the datasheet's 0,1,2,1,2 example), the pacing rule, FIFO enable/SHIFT/ERR/OVER, DMA settings (THRESH 1, DREQ_EN), the temperature channel as channel 4.
- **Left, deliberately:** `CS.TS_EN` does not gate channel 4 (the datasheet does not say what an unbiased sensor reads), the sample error flag (SAR non-convergence) is never raised by the default device, INL/DNL/ENOB (4.9.3-4.9.4) are analogue characteristics of the silicon.
- Tests: two datasheet tests in `tests/test_adc.py`, two C++ checks, and the oracle's new mutants `fifo_read_no_irq`, `multi_shot_ignores_en`, `c_multi_shot_ignores_en`.


### PWM (datasheet 4.5)

Read: 4.5.1-4.5.3 and the CSR/DIV/CTR/CC/TOP/EN/INT* tables. The period formula is an image in the PDF (the text extraction drops it); it was read from the rendered page: **period = (TOP + 1) x (CSR_PH_CORRECT + 1) x (DIV_INT + DIV_FRAC / 16)**. The tool had not checked the per-slice registers at all (their tables are headed `CH0_CSR, CH1_CSR, ..., CH7_CSR` and `Offsets: 0x00, 0x14, ...`); it does now.

- **Phase-correct mode was wrong in two ways - this resolves the "bug 4" that 0096 left in the backlog for the datasheet.** (1) The output: the wrap did not drive A/B in phase-correct mode and the compare alarms only ever *cleared* them, so a phase-correct output went low at the first compare and never rose again. (2) The period: the reference ran the counter as a zigzag of `2 * TOP` counts; the datasheet's period is `2 * (TOP + 1)` (the 0 and the TOP are each held for two counts - "the 0 to 0 count transition"). Both are fixed by changing the model, not by patching the symptoms: the slice's counter is **always an INCREMENT timer of one period of counts** (`TOP + 1`, or `2 * (TOP + 1)` in phase-correct mode), the output is **high while the counter is below CC** (Figure 104: "When the input value is higher than the counter, the output is driven high"), so an output *falls* when the counter reaches CC and, in phase-correct mode only, *rises* again on the way down through CC - 1; the wrap (the 0 to 0 transition) raises it and updates the latched copies. Two new compare alarms per slice (`alarm_a_rise`, `alarm_b_rise`) carry the rising events; a CC above TOP is never reached (100 %), a CC of 0 is never above the count (0 %). `CTR` shows the position on the triangle (0..TOP, TOP..0). The `Timer32` ZIGZAG mode is no longer used by the PWM (it stays in `Timer32`, tested on its own).
- **The internal copies of CC and TOP are explicit now** (`latched_cc_a/b`, `latched_top`), updated at the wrap or the enable ("updated from the first register at the instant the counter wraps"), so a change of PH_CORRECT while running recomputes the period and the events from the latched values.
- **`CSR` stored bits 31:8** (reserved) and **`DIV` bits 19:12**: now 0x3F (DIVMODE, B_INV, A_INV, PH_CORRECT, EN) and 0xFFF (INT 11:4, FRAC 3:0).
- **`PH_ADV` at full speed:** "Counter must be running at less than full speed (div_int + div_frac / 16 > 1)" - ignored now when the divider is 1 (there is no gap in the clock enable to insert a pulse into). The three `tests/test_pwm.py` tests that used it at DIV 1 now run at DIV 2 (rp2040js 1.4.0's fix was tested at full speed).
- **Checked and consistent:** the pin mapping (GPIO n to slice (n/2) mod 8, A/B by parity; both pins of the pair 16 apart carry the same signal; B inputs OR-ed), `EN` as an alias of the CSR_EN bits, `INTR` write-1-to-clear, one IRQ per wrap and the DREQ pulse, `DIV_INT` 0 meaning 256, the 100 % / 0 % rules, double buffering at the wrap in free-running mode.
- **Left, deliberately:** the fractional divider is an average, not the first-order delta-sigma; the exact counting of edges against the divider in the level/edge modes is as before; the A pin of a slice that is disabled keeps its last level.
- Tests: `tests/test_pwm_phase_correct.py` (nine datasheet-derived scenarios counted in clk_sys cycles: the hello_pwm example, the triangle, duty, 0/100 %, the IRQ at the 0 to 0 transition, double buffering at the wrap, the period formula with a fractional divider, PH_ADV, reserved bits), the oracle's mutants (the 16 that referred to the old text rewritten, about 20 new for the phase-correct events and the masks), `tests/cpp/test_pwm.cpp` against regenerated vectors (`tests/utils/pwm_cpp_vectors.py`).

### UART (datasheet 4.2, the PL011)

Read: the UARTDR, UARTRSR, UARTFR, UARTLCR_H, UARTCR, UARTIFLS, UARTIMSC, UARTRIS, UARTICR, UARTDMACR tables and the ID registers (the PERIPHID/PCELLID values the reference returns are right: `0x11 0x10 0x34 0x00`, `0x0D 0xF0 0x05 0xB1`; the tool's PCELLID3 "reset" finding is its own misparse).

- **Reserved bits stuck:** `UARTLCR_H` kept bits 31:8 and `UARTCR` bits 31:16 and 6:3. Now 0xFF and 0xFF87.
- **Missing registers, now there:** `UARTIFLS` (6 bits, reset 0x12), `UARTILPR` (8 bits), `UARTDMACR` (3 bits), `UARTRSR`/`UARTECR` (reads the error flags, a write clears them). `IFLS` and `ILPR` are **stored and not acted on**: the RX interrupt still comes with every byte (a superset of every trigger level: a driver that drains the FIFO in its handler behaves the same, and the receive-timeout interrupt that a trigger level would need is never needed), the TX FIFO never fills, there is no IrDA.
- **The DMA requests were wrong.** The reference raised the TX DREQ when UARTEN was set (whatever DMACR said) and never the RX one, so a DMA channel paced by `DREQ_UARTn_RX` could not run. Now TX asks while UARTEN, TXE and `DMACR.TXDMAE` are set (the FIFO never fills), and RX while UARTEN, RXE and `RXDMAE` are set and the FIFO holds a byte, unless `DMAONERR` is set and an error interrupt is up ("the DMA receive request outputs ... are disabled when the UART error interrupt is asserted"). Both are re-announced (TX then RX) on every change of CR, DMACR, the FIFO's emptiness, the error interrupts and at reset (a reset used to leave a stale request up). pico-sdk's `uart_init` writes `DMACR` with both enables (`src/rp2_common/hardware_uart/uart.c`, unless `PICO_UART_NO_DMACR_ENABLE`), so real firmware is unaffected.
- **A disabled UART still sent and received.** The datasheet: "TXE: ... If this bit is set to 1, the transmit section of the UART is enabled", RXE likewise, UARTEN gates both. A `UARTDR` write now sends only with UARTEN and TXE, and a byte fed from the wire is taken only with UARTEN and RXE (UARTEN is 0 after a reset until the firmware sets it; the tests that feed or send now enable the UART as a firmware does).
- **Overrun:** a byte that arrives with the 32-entry FIFO full is dropped as before, and now also sets `UARTRSR.OE` and the raw OE interrupt (bit 10 of `UARTRIS`), cleared by `UARTICR` / `UARTECR` ("no more data is written when the FIFO is full").
- **Checked and consistent:** the baud divisors (`IBRD` 16 bits, `FBRD` 6 bits, `baud = clk_peri / (16 x (IBRD + FBRD / 64))`), the interrupt mask/status/clear layout, `UARTFR` (TXFE and RXFE at reset; TXFF and BUSY never set because the transmitter is instant), the ID registers.
- **Left, deliberately:** the RX FIFO is 32 deep whatever `FEN` says (the datasheet: with FIFOs disabled it is a 1-byte holding register; but bytes arrive here in instant bursts that a real line would space at the baud rate, so a 1-deep register would drop what hardware would not); no break/parity/framing errors, no modem lines, no timing (loopback, `UARTCR.LBE`, is modelled) of transmission.

#### UART, second pass (2026-10-12): the operation chapters 4.2.2-4.2.6

Read the interrupt, FIFO and error rules against `_uart.py` and `core/uart.hpp`. **Nothing changed in code**; what the reading found is a list of gaps, none of them a wrong value firmware can observe through the paths it uses:

- **`UARTRTINTR` (receive timeout) is never raised.** The datasheet: set when the RX FIFO is not empty and no data has arrived for a 32-bit period; cleared by emptying the FIFO or by `UARTICR`. It needs a time base (baud rate, `clk_peri`) the UART block does not have yet. It stays a gap, not a bug, because `RXINTR` fires on every byte (see the IFLS note), so a driver waiting on the timeout to flush a partial trigger level is woken anyway. Becomes real work together with a modelled trigger level.
- **`UARTDR[11:8]` (OE/BE/PE/FE flags of the entry) always read 0.** The overrun flag is only in `UARTRSR` and the raw OE interrupt. The datasheet copies OE into the FIFO entry when a slot frees; the reference drops the byte and keeps no per-entry flags. pico-sdk's `uart_getc` and the MicroPython/CircuitPython ports read `DR` as a byte, so nothing consumes them.
- **`UARTTXINTR` is set by each `UARTDR` write and cleared by `UARTICR`** rather than being the level/transition rule of 4.2.6 (level when the TX FIFO is at or below the trigger level, raised only after written data has left). The transmitter is instant, so the sequence a driver sees (write, interrupt, clear) is the same.
- **Checked and consistent:** `RXINTR` follows FIFO non-emptiness and drops when `UARTDR` empties the FIFO; `UARTIMIS = RIS & IMSC`; `UARTICR` bits 10:0 clear the raw status; the error interrupts gate the RX DREQ under `DMAONERR`; `UARTIMSC` keeps 11 bits.
- Tests: `tests/test_uart.py` (seven datasheet cases), `tests/cpp/test_uart.cpp` rewritten around the new DREQ rule and the gating, the oracle (`tests/utils/uart_diff.py`: the new registers in the stream, an enabling scenario so that traffic passes, 47 mutants - the three obsolete CR-DREQ ones replaced by 14 DREQ/gating/overrun/register mutants).

### SPI (datasheet 4.4, the PL022)

Read: 4.4.2 (the block), 4.4.3.2-4.4.3.6 (configuring, enabling, clock ratios, SSPCR0/SSPCR1, bit rate), the DMA interface text (4.4.3.16) and every register table (SSPCR0 .. SSPDMACR, the ID registers; the values the reference returns for those, `0x22 0x10 0x34 0x00` and `0x0D 0xF0 0x05 0xB1`, are right - the tool's PCELLID3 "reset" finding is its own misparse).

- **Reserved bits stuck:** SSPCR0 kept bits 31:16, SSPCR1 and SSPIMSC bits 31:4, SSPDMACR bits 31:2. Now 0xFFFF, 0xF, 0xF, 0x3. (SSPCPSR's bit 0 "not sticking" is right: "the least significant bit always returns zero on reads"; the reference already masked 0xFE.)
- **A disabled SSP sent.** "You can prime the transmit FIFO ... when the PrimeCell SSP is disabled ... Once enabled, transmission or reception of data begins" (4.4.3.3): the reference pushed a word to the device as soon as it was written, whatever SSE said. Now nothing leaves while SSE is 0 and a primed FIFO starts when SSPCR1 enables it.
- **The DMA requests ignored SSE and SSPDMACR.** "All request signals are deasserted if the PrimeCell SSP is disabled, or the DMA enable signal is cleared" (4.4.3.16): the reference kept the TX request up whenever the FIFO had room and the RX one whenever it had a word. Now TX needs SSE and TXDMAE, RX needs SSE and RXDMAE, and both are re-published on every write of SSPCR1 and SSPDMACR as well as on every FIFO change. pico-sdk's `spi_init` sets both SSPDMACR enables ("Always enable DREQ signals -- harmless if DMA is not listening") and then SSE - read from `src/rp2_common/hardware_spi/spi.c` (master) - so real firmware is unaffected.
- **Loop back (SSPCR1.LBM) was not implemented.** "Output of transmit serial shifter is connected to input of receive serial shifter internally": the word written to SSPDR now comes straight back into the RX FIFO and does not reach the device.
- **`SSPRIS` reset:** the datasheet has TXRIS = 1 at reset (the TX FIFO is empty, "half empty or less"); the reference started at 0 and only set it after the first FIFO change. Fixed at power-on and at `reset()`.
- **Checked and consistent:** the FIFOs (16 bits wide, 8 deep), the interrupt thresholds (TX at 4 or fewer, RX at 4 or more), the overrun interrupt, SSPICR clearing only RT and ROR, `SSPSR` (BSY while a frame is in flight or the TX FIFO is not empty), the bit-rate formula `clk_peri / (CPSDVSR x (1 + SCR))`, data size = DSS + 1.
- **Left, deliberately:** the frame format (FRF, TI/Microwire), the receive-timeout interrupt (RT never rises), slave mode (MS and SOD are stored; the block still masters), the burst DMA requests (the chip has one request per direction), the rule that MS can be changed only with SSE = 0, and the DSS values 0-2 (reserved, "undefined operation": treated as 1-3 bits).
- Tests: `tests/test_spi.py` (four datasheet cases), `tests/cpp/test_spi.cpp` (the three new behaviours, the old checks now run with the SSP enabled as a firmware has it), the oracle (`tests/utils/spi_diff.py`: SSPDMACR values in the stream, 13 new mutants for the masks, the gating, the loop back, the start on enable and the reset value; `imsc_masked`, which the fix made equal to the reference, became `imsc_unmasked`).

### I2C (datasheet 4.3, the DW_apb_i2c)

Read: every register table (IC_CON .. IC_COMP_TYPE) and the description text in them; **not** read: the protocol chapters (10-bit addressing sequences, the abort procedures, spike suppression, the clock-count formulas) - the master's bus sequencing is as before.

- **`FIRST_DATA_BYTE` was bit 10; the datasheet has it at bit 11** of what IC_DATA_CMD reads (bit 10 is RESTART, of a write). A driver testing the flag saw the wrong bit.
- **The configuration registers were writable while enabled.** IC_CON, IC_TAR, IC_SAR, the four SCL counts, IC_SDA_HOLD, IC_FS_SPKLEN: "can be written only when the DW_apb_i2c is disabled ... Writes at other times have no effect". Now gated on IC_ENABLE[0]. pico-sdk's `i2c_init`, `i2c_set_baudrate` and `i2c_write_blocking` all clear `enable` before writing `con`/`tar`/the counts (`src/rp2_common/hardware_i2c/i2c.c`, master), so real firmware is unaffected.
- **Widths and minima:** IC_CON 9:0 (bit 10 is read only), IC_TAR 11:0 (SPECIAL and GC_OR_START are stored; the general-call/START-byte behaviour they select is not modelled), IC_SDA_HOLD 23:0 (was a constant 1 with a warning), IC_ENABLE 2:0, IC_DATA_CMD 10:0 of a write. The SCL counts have minima - "hardware prevents values less than this being written, and if attempted results in 6 (8) being set" - and IC_FS_SPKLEN's is 1 (a 0 used to be ignored, it now sets 1).
- **Registers that were unimplemented, now stored** with their datasheet reset values and masks and **not acted on**: IC_SDA_SETUP (0x64), IC_ACK_GENERAL_CALL (1), IC_SLV_DATA_NACK_ONLY, IC_DMA_CR, IC_DMA_TDLR, IC_DMA_RDLR; IC_CLR_RESTART_DET clears RESTART_DET (nothing in a master raises it). There is no slave and no I2C DREQ in the chip, so the DMA registers are inert: **DMA-driven I2C is a feature, in the backlog**.
- **RX_FULL is a level:** "set when the receive buffer reaches or goes above the RX_TL threshold ... automatically cleared by hardware when buffer level goes below the threshold". The reference cleared it on *any* read of IC_DATA_CMD. Now re-evaluated on every push, pull, flush and IC_RX_TL write. **TX_EMPTY** is cleared by a write only when the level goes above IC_TX_TL.
- **A transmit abort flushes the RX FIFO as well** ("flushes/resets/empties the TX_FIFO and RX_FIFO whenever there is a transmit abort"; IC_RXFLR "is cleared ... whenever there is a transmit abort").
- **ACTIVITY was never raised.** Now set when the master starts, cleared by IC_CLR_ACTIVITY / IC_CLR_INTR "if the I2C is not active anymore" (not while it is), and by disabling.
- **Checked and consistent:** the FIFO depths (16), IC_STATUS composition, IC_TXFLR/IC_RXFLR, the abort-source bits and the accumulate-until-read rule, IC_INTR_MASK reset 0x8FF, IC_COMP_VERSION/TYPE.
- **Left, deliberately:** slave mode (RD_REQ, RX_DONE, GEN_CALL, RESTART_DET never rise), the general call / START byte / high-speed sequences, TX_EMPTY_CTRL, RX_FIFO_FULL_HLD_CTRL, STOP_DET_IF_MASTER_ACTIVE, the "FIFOs stay flushed until IC_CLR_TX_ABRT is read" rule, TX_OVER/RX_OVER/RX_UNDER keeping their level until idle, IC_ENABLE_STATUS bits 2:1.
- Tests: `tests/test_i2c.py` (three datasheet cases), `tests/cpp/test_i2c.cpp` (one directed test for the new behaviour, the old checks reordered to configure before enabling), the oracle (`tests/utils/i2c_diff.py`: the bring-up stream now disables, configures and enables as pico-sdk does, the new registers in the snapshot, 24 new mutants).

### I2C, second pass: 4.3.10.3 / 4.3.10.4 (disabling and aborting), 4.3.11-4.3.16 (2026-10-11)

Read against the reference (`_i2c.py`, `core/i2c.hpp`): the master-side rules of the chapters the first pass skipped; the slave chapters (4.3.10.1) and the bus-clear and spike-suppression circuitry have no counterpart in a model that does not clock a wire.

- **Fixed: `IC_ENABLE.ABORT` left the bus held and the bit stuck.** Datasheet (4.3.10.4, table 477): "In response to an ABORT, the controller issues a STOP and flushes the Tx FIFO after completing the current transfer, then sets the TX_ABORT interrupt after the abort operation. The ABORT bit is cleared automatically after the abort operation"; 4.3.10.3: a master that has sent a command without STOP "continues to remain active, holding the SCL line low until a new command is received ... you can issue the ABORT (IC_ENABLE.ABORT) to relinquish the I2C bus". The reference flushed the FIFOs and raised TX_ABRT at the moment of the write, and issued the STOP only if a command happened to be in flight - for a master *waiting* for its next command (the very case the datasheet names) no STOP was ever issued, `ABORT` read 1 for ever and the device never saw the bus released (shown by a four-line script: `ENABLE|ABORT` after a byte without STOP left `IC_ENABLE` = 3 and no `stop` event). Now: ABORT sets a pending abort, forces STOP on the current transfer (a waiting master issues it at once), and when that STOP is done (`complete_stop`) the FIFOs are flushed, `ABRT_USER_ABRT` and TX_ABRT raised and the bit cleared; the flush count is the commands still queued at that moment.
- **Fixed: ABORT is ignored unless ENABLE was already set** ("The software can set this bit only when ENABLE is already set; otherwise, the controller ignores any write to ABORT bit"): a write that sets ENABLE and ABORT together, or ABORT alone while disabled, no longer aborts. A second write while an abort is in progress aborts nothing more.
- Tests: `tests/test_i2c.py` (three cases, both builds), `tests/cpp/test_i2c.cpp` (the old checks encoded the old behaviour and were rewritten), the oracle (`tests/utils/i2c_diff.py`: `_abort_pending` in the compared state, TX_CMD_BLOCK and RX_TL macros in the stimulus - a mutant that had been caught only by accident of the old stream needed them -, the four ENABLE-write mutants rewritten as single changes of the new logic and seven new ones for the abort rules).
- **Checked and consistent:** Table 451 (which interrupts are set by hardware and cleared by software: only TX_EMPTY and RX_FULL are levels - first pass), IC_FS_SPKLEN's minimum of one and "writable only while disabled" (first pass), the DMA registers (stored; "DMARDLR/TDLR can be left at reset values of zero", the interface itself is the backlog item), the abort-source and flush-count rules.
- **Not modelled, unchanged:** disabling is instantaneous (a real master can be disabled only after a command with STOP; until then IC_ENABLE_STATUS keeps reading 1 and SCL stays low), spike suppression and its two-flip-flop synchroniser, Fast Mode Plus timing (IC_CLK >= 32 MHz), the SDA-stuck-low recovery (nine clock pulses, 4.3.13) and the clock-count formulas of 4.3.14: the model has no wire to apply them to.

### DMA sniffer, second pass: 2.5.5.2 and SNIFF_CTRL / SNIFF_DATA (2026-10-11; read, nothing changed)

Read: the CRC section of 2.5.5.2, tables 146 and 147 and `CTRL.SNIFF_EN` (bit 23). The reference has the register offsets (`SNIFF_CTRL` 0x434, `SNIFF_DATA` 0x438) and the `SNIFF_EN` constant and nothing else: both registers warn "unimplemented", and `SNIFF_EN` is stored with the rest of CTRL and ignored. That is the backlog item ("DMA | the sniffer ...") and it stays a feature, not a correction; what reading it adds:

- **The register file is fully specified, the behaviour is not.** `SNIFF_CTRL` is 12 bits - `EN` (0), `DMACH` (4:1), `CALC` (8:5: 0 CRC32, 1 CRC32R, 2 CRC16, 3 CRC16R, 0xE EVEN, 0xF SUM), `BSWAP` (9), `OUT_REV` (10), `OUT_INV` (11), all reset 0; `SNIFF_DATA` is the 32-bit seed/accumulator. The datasheet says the unit is "purely passive", sees the data **after** the channel's own byte swap (so `CTRL.BSWAP` and `SNIFF_CTRL.BSWAP` cancel), advances only for transfers of a channel with `SNIFF_EN` set that is the selected `DMACH`, and that `OUT_REV`/`OUT_INV` only transform what a *read* returns, not the accumulator.
- **What the datasheet does not give** (so, by the 3g rule, a sniffer cannot be written from it alone): the polynomial's bit order and how an 8-, 16- or 32-bit transfer enters the 32-bit register (the "R" variants "with bit reversed data"), the CRC-16 register width and where its result sits in `SNIFF_DATA`, and the seed handling. Real firmware depends on it: the bootrom checks the stage-2 boot block with exactly this unit. The sources to take it from are pico-sdk (`hardware_dma`'s `dma_sniffer_*`, `boot_stage2`'s checksum tool, which must reproduce the hardware's CRC-32) and a test vector from a real boot block; they were not fetched in this pass (the tool path tried returned 404).
- **Decision needed before any code:** implement the sniffer as a feature (reference first, then C++ with the oracle, a hardware-sourced CRC vector in the test), or leave it in the backlog until a guest uses it. Nothing here uses it today: the boot paths exercised (MicroPython, CircuitPython, Kaluma, Pico SDK, the CYW43 scan) do not touch `SNIFF_*`.

### CLOCKS, second pass: the frequency counter 2.15.4 (implemented, 2026-10-11) and the resus 2.15.5 (read, left)

- **Implemented: the frequency counter `FC0_*`** (reference only: CLOCKS stays Python by the decision of 0096). Before, `FC0_REF_KHZ` .. `FC0_RESULT` (0x80..0x9C) were "unimplemented" and pico-sdk's `frequency_count_khz()`, which busy-waits on `FC0_STATUS.DONE`, never returned. Now: the eight registers with the widths and reset values of tables 238-246 (REF_KHZ 19:0, MIN_KHZ 24:0, MAX_KHZ 24:0 reset 0x1FFFFFF, DELAY 2:0 reset 1, INTERVAL 3:0 reset 8, SRC 7:0, STATUS and RESULT read only); a write to `FC0_SRC` starts a count (it runs for NULL too, per the SDK's own comment) that is RUNNING for `FC0_DELAY` periods of clk_ref (from `FC0_REF_KHZ`) plus `0.98 us * 2**FC0_INTERVAL` of simulated time and then DONE; the result is the source's frequency in kHz with five fraction bits (KHZ 29:5, FRAC 4:0), so `result >> 5` is what the SDK returns; `FC0_MIN_KHZ`/`FC0_MAX_KHZ` give PASS, SLOW or FAST, and DIED (a clock stopped, NULL, or a GPIN source that is not modelled) with FAIL = DIED | FAST | SLOW. The sources (table 243): both PLLs, ROSC and ROSC_PH, XOSC, GPIN0/1 (give 0), clk_ref, clk_sys and clk_peri (the frequency the chip runs at: 125 MHz until firmware configures the tree), clk_usb, clk_adc and clk_rtc. clk_usb and clk_adc had no frequency in the model; they now have `usb_freq`/`adc_freq` through the same aux mux as clk_rtc (tables 230, 232, 234), registers still stored as before. **Not independently sourced:** the result is exact (real hardware is only as accurate as table 206), DONE is also set for a DIED test (the datasheet does not say, and the SDK would wait for ever), WAITING is never set, and the DELAY/INTERVAL/REF/SRC configuration is read when the count starts. Tests: `tests/test_clocks_datasheet.py` (six cases, both builds).
- **The resus has nothing to detect in this model.** It switches clk_sys to clk_ref when no clk_sys edge is seen for a programmed interval; clk_sys here is a number that is never "stopped" (a frequency of 0 keeps the last value, see `update_clocks`). A resus would first need a stopped-clock model. The datasheet itself calls it "a debugging aid". Left in the backlog.

### SIO (datasheet 2.3.1 and 2.3.1.7)

Read: 2.3.1.1-2.3.1.6 (CPUID, GPIO, spinlocks, FIFOs, divider, interpolator including the lane, blend and clamp descriptions) and every register table. The interpolator's worked examples (pico-examples `hello_interp`: the times table, the moving mask with and without sign extension, the lane cross-over, blend 500..998, clamp) are now tests with the output the datasheet prints, and **the reference already produces all of it** - the lane arithmetic is right.

- **`GPIO_HI_OUT` / `GPIO_HI_OE` were 30 bits;** the QSPI bank has six pins (5:0). Now 0x3F.
- **The divider's reset state:** `DIV_xDIVISOR` reset to 1 and `DIV_CSR` to 0 (READY low, so a driver polling READY before its first division would never see it); the datasheet has the divisor at 0 and READY at 1.
- **`INTERPx_ACCUMy_ADD` took 32 bits;** the register is 23:0 (datasheet; `SIO_INTERP0_ACCUM0_ADD_BITS 0x00ffffff` in pico-sdk's `sio.h`, `interp_add_accumulator` just stores a `uint32_t`).
- **`FORCE_MSB` was taken from lane 0's CTRL for both lanes,** and the forced bits were written back into the accumulators by a POP. Each lane has its own field ("ORed into bits 29:28 of the lane result presented to the processor on the bus. No effect on the internal 32-bit datapath"), so lane 1 now uses CTRL_LANE1 and the accumulators take the datapath value.
- **Checked and consistent:** spinlocks (read claims, any write releases, SPINLOCK_ST), the GPIO SET/CLR/XOR aliases, the divider's signed/unsigned truncation and the quotient/remainder signs, the 8-cycle charge, DIRTY/READY handling, the three lane results, blend/clamp availability per interpolator, the CTRL layout.
- **Not sourced, left as is:** the quotient and remainder of a **divide by zero** (the datasheet does not say; the reference tests the unmasked dividend with `> 0`, so a negative signed dividend and a zero dividend probably differ from silicon), what a read of a `SET`/`CLR`/`XOR` alias returns (0, "TODO verify with silicon").
- **Left, deliberately:** the inter-core FIFO and core 1 (record 0053), CPUID is always 0, the two cores' simultaneous writes.

### DMA (datasheet 2.5)

Read: 2.5.2 (aliases, triggers, chaining, null triggers), 2.5.3.1 (the DREQ table - matches the enum), 2.5.4 and the controller-level register tables (INTR .. N_CHANNELS); **not** read: the per-channel CTRL table text beyond the field names, 2.5.5 (the DMA cycle model: FIFOs, bus arbitration, priority), 2.5.6 (the examples), the sniffer's description.

- **The pacing timers' period was inverted:** the datasheet has "TREQ assertions at a rate set by ((X/Y) * sys_clk) ... can only generate TREQs at a rate of 1 per sys_clk (i.e. permanent TREQ) or less", i.e. a period of Y/X cycles, at least one; the reference used X/Y cycles, so `dma_timer_set_fraction(t, 1, 32768)` ran 32768 times too fast. A timer with X or Y at 0 asserts nothing.
- **TIMER3's X was read from bit 4** (a JavaScript shift-by-36 inherited from rp2040js, flagged in the code as "almost certainly an upstream bug"); X is bits 31:16 of all four timers.
- **`CHAN_ABORT`, `MULTI_CHAN_TRIGGER` (self-clearing) and `FIFO_LEVELS` (debug) read as unimplemented** - all ones and a warning. The datasheet says of CHAN_ABORT "After writing, this register must be polled until it returns all-zero. Until this point, it is unsafe to restart the channel", so a firmware that follows it would spin on 0xFFFFFFFF. (pico-sdk's own `dma_channel_abort` does not do that: it polls `CTRL_TRIG.BUSY` of the channel, which the model clears at once - checked in `hardware_dma/dma.h`; an earlier version of this note said the SDK polled CHAN_ABORT, from memory, and was wrong.) They read 0 now (an abort is flushed at once here, and there are no address/data FIFOs to fill).
- **Checked and consistent:** the trigger rules (no start when disabled, already running or a zero write), the chain rules (no self-chain, immediate re-trigger with TRANS_COUNT reload), IRQ_QUIET and the null trigger, INTR/INTS write-1-to-clear, INTE/INTF 16 bits and `INTS = (INTR & INTE) | INTF`, the DREQ numbering, EN cleared while BUSY pausing the channel, N_CHANNELS = 12.
- **Not implemented, in the backlog:** the sniffer (SNIFF_CTRL, SNIFF_DATA, CTRL.SNIFF_EN), HIGH_PRIORITY and the arbitration, the read/write/AHB error flags, the DBG_CTDREQ counter (never counts), a transfer taking bus cycles (one transfer per alarm).
- Tests: `tests/test_sio_datasheet.py` (the interpolator examples and the new widths), `tests/cpp/test_sio.cpp`, `tests/cpp/test_dma.cpp` (the timers now follow Y/X), the DMA oracle (`tests/utils/dma_diff.py`: `timer3_shifts_16` became three mutants, `timer3_shifts_4`, `timer_period_inverted` and `timer_rate_uncapped`, plus `abort_read_warns`).

### SSI (datasheet 4.10, the XIP SSI)

Read: every register table of the SSI (CTRLR0 .. TXD_DRIVE_EDGE) and the field lists; **not** read: the operation chapters (the FIFO and interrupt behaviour of the DW_apb_ssi, the XIP streaming and its DREQs, the dual/quad formats). The model is a **flash-command emulation** driven by the bootrom through DR0 (framed by QSPI_SS, not by SER/SSIENR, as the code explains); its register file was never the point, which is why most of the findings are widths.

- **Reserved bits stuck:** CTRLR0 (23 and 31:25), CTRLR1 (31:16), SSIENR (31:1), BAUDR (31:16) and SPI_CTRLR0 (31:24 is XIP_CMD, but 23:19, 10 and 7:6 are reserved) kept whatever was written. Now 0x017FFFFF, 0xFFFF, 0x1, 0xFFFF and 0xFF07FB3F.
- **`SPI_CTRLR0` reset value** is 0x03000000 (XIP_CMD = 0x03, the READ command); it was 0.
- **`TXFLR` was writable** (it is read only); a write is ignored now (it always reads 0: there is no TX FIFO to fill).
- **Unimplemented, now stored and not acted on:** MWCR, SER, TXFTLR, RXFTLR, IMR, DMACR, DMATDLR, DMARDLR (their widths, reset 0). ISR, RISR and the clear-on-read registers (TXOICR, RXOICR, RXUICR, MSTICR, ICR) read 0: nothing is raised.
- **Checked and consistent:** IDR 0x51535049 and SSI_VERSION_ID 0x3430312A, SR's TFE/TFNF/RFNE composition (the reset column of a status register is a placeholder), RXFLR as the length of the model's RX queue.
- **Left, deliberately:** the SSI's FIFOs (the RX queue is unbounded, TX is instant, so TFE/TFNF are constant and RFF/BUSY/TXE/DCOL never set), the interrupts, DMA and XIP streaming, the dual/quad formats and the DDR bits (SPI_CTRLR0 is stored only), CTRLR0.DFS/TMOD and the rest of the frame setup (the model shifts 8 bits whatever they say).
- Tests: `tests/test_ssi.py` (one datasheet case), `tests/cpp/test_ssi.cpp`, the oracle (`tests/utils/ssi_diff.py`: the new registers in the write stream, 10 new mutants for the masks, the reset values, TXFLR and the stored registers).

### CLOCKS and PLL (datasheet 2.15 and 2.18)

Read: the register tables of CLOCKS (CLK_GPOUT0_CTRL .. INTS) and PLL, the AUXSRC/SRC encodings of REF, SYS and PERI against the code (they match); **not** read: the clock-generator chapters (glitchless mux behaviour, the frequency counter's operation, the resus), and the PLL's VCO limits and start-up. Both are plain Python blocks (no C++ twin, no oracle) - the tests are `tests/test_clocks_datasheet.py`.

- **`CLK_REF_DIV`, `CLK_ADC_DIV` read through a mask of bits 5:4** (`& 0x30`) where the INT field is bits 9:8, so they read 0 after reset (the datasheet: 0x100) and lost the written value; `CLK_USB_DIV` was not masked at all and `CLK_RTC_DIV` (a full 24.8 divider) read through the same wrong mask. Now REF/USB/ADC keep bits 9:8 (written and read) and RTC reads whole.
- **`CLK_SYS_RESUS_CTRL` read a constant 0xFF and ignored writes;** it is CLEAR 16, FRCE 12, ENABLE 8, TIMEOUT 7:0 (reset 0xFF) and is stored now (the resus itself is not modelled).
- **Unimplemented, now stored and not acted on:** WAKE_EN0/1 and SLEEP_EN0/1 (reset all ones, widths 32 and 15 bits), INTE/INTF (the resus interrupt's one bit); INTR reads 0 and INTS is INTF (nothing raises INTR; the CLOCKS IRQ line is not driven).
- **PLL registers stored every bit written:** CS keeps BYPASS and REFDIV (LOCK is read only and always set), PWR 0x2D, FBDIV_INT 11:0, PRIM POSTDIV1/POSTDIV2 (0x77000). (The tool's PRIM "reset" finding is its misparse of a later table.)
- **Checked and consistent:** the SRC/AUXSRC encodings of CLK_REF, CLK_SYS and CLK_PERI, the divider decoding (INT 0 = 2^16), the *_SELECTED one-hot values, the PLL output formula `f_ref / REFDIV x FBDIV / (POSTDIV1 x POSTDIV2)` and the bypass case.
- **Not sourced / left, deliberately:** `CLK_PERI_DIV` (0x4C) exists in the model and not in the datasheet (clk_peri has no divider; the register is stored and ignored); the PLL is always locked, even with PD or VCOPD set (no start-up time; pico-sdk only waits for LOCK after powering the PLL up); the frequency counter FC0_* (a firmware that measures a clock would wait forever: it is a feature, in the backlog) and ENABLED0/1 stay unimplemented; the clock gating of WAKE_EN/SLEEP_EN is not modelled.

### PIO (datasheet 3.4 and 3.5, and the register tables of 3.7)

Read: the instruction set (3.4.2-3.4.10: every encoding and operation), 3.5.1 (side-set, skimmed), 3.5.3 (FIFO joining), 3.5.4 (autopush and autopull with their pseudocode), 3.5.5 (clock dividers, skimmed), 3.5.7 (forced and EXEC'd instructions) and the SMx_CLKDIV, SMx_EXECCTRL, SMx_SHIFTCTRL, SMx_PINCTRL tables; **not** read: the rest of 3.5.6 (GPIO mapping and output priority), the programming examples, the system-level registers other than the tool's pass (CTRL, FSTAT, FDEBUG, FLEVEL, IRQ*, DBG_*, INTR/INTE/INTF/INTS: the tool found nothing but its own artefacts - `DBG_CFGINFO` 0x00200404 is FIFO_DEPTH 4, SM_COUNT 4, IMEM_SIZE 32, right). The instruction decoding, the JMP conditions, WAIT sources and polarity, the IRQ index decoding, MOV operations, the side-set field split and the wrap rules all match.

- **`PUSH IFFULL` and `PULL IFEMPTY` only tested the threshold with autopush / autopull on.** The datasheet: "IfFull: do nothing unless the total input shift count has reached its threshold ... (the same as for autopush)" and "PUSH IFFULL helps to make programs more compact, like autopush. It is useful in cases where the IN would stall at an inappropriate time **if autopush were enabled**"; likewise IfEmpty. The shift counts are kept whether or not auto-transfer is on, so a program using `pull ifempty` without autopull pulled every time. Now the threshold is always tested.
- **With autopull on, a `PULL` is a no-op while the OSR is full** ("it becomes a no-op if the OSR is full ... a fence"; the OSR is full when its shift count is 0). It pulled a second word - one that is then lost - right behind an autopull.
- **`OUT x, 32` left the OSR as it was;** all 32 bits are shifted out and the OSR shifts in zeroes, so it is empty. A `MOV x, OSR` afterwards read the stale word.
- **A reset state machine had an OSR shift count of 0 (full);** "At reset, or upon CTRL_SM_RESTART assertion, ISR shift counter is set to 0 ..., and OSR to 32 (nothing left to be shifted out)". `restart()` already did; construction and `reset()` did not (pico-sdk's `pio_sm_init` restarts every machine, which hid it).
- **`CTRL.SM_RESTART` left EXEC_STALLED set** ("any stalled instruction written to SMx_INSTR or run by OUT/MOV EXEC" is cleared).
- **Reserved bits stuck:** SMx_EXECCTRL 6:5 and SMx_SHIFTCTRL 15:0 were stored (now 0x7FFFFF9F and 0xFFFF0000; EXEC_STALLED stays read only).
- **Not implemented, in the backlog (features, not corrections):** **`SHIFTCTRL_FJOIN_TX` / `FJOIN_RX`** (done 2026-10-12, see the section below), **`EXECCTRL_OUT_STICKY`, `INLINE_OUT_EN`, `OUT_EN_SEL`** (the existing TODO), the input synchroniser bypass effect.
- **Left, deliberately:** autopull is done when the next OUT starts, not right after the OUT that reaches the threshold or "at any point between two OUTs when data arrives" (3.5.4.2), so the TX FIFO drains later than on silicon by up to one OUT; an OUT that finds the OSR exhausted pulls and shifts in the same cycle, where the datasheet stalls that cycle ("it cannot fill an empty OSR and OUT it on the same cycle"). The cycle counts of streaming programs are the same, FIFO levels read slightly high, and a change needs a design of its own (the pacing of `due` times, 0063).
- Tests: `tests/test_pio_datasheet.py` (five cases on both implementations), `tests/cpp/test_pio.cpp` (one directed test, the register-mask checks updated). The PIO oracle has no logic mutants (it compares the two implementations only), so there are none to add.

### PIO, second pass: the system-level registers (tables 371-387, 2026-10-12)

The first pass left CTRL, FSTAT, FDEBUG, FLEVEL, IRQ, IRQ_FORCE, INPUT_SYNC_BYPASS, DBG_* and INTR / IRQ0,1_INTE / INTF / INTS to the tool, which only reads the first register of a family. Read against the tables and probed through the bus on both builds (the probes agreed, the two implementations being held together by the oracle):

- **Fixed: `IRQ_FORCE` stored bits 31:8 in `IRQ`.** IRQ and IRQ_FORCE are 7:0 (reserved above); a write of ones left `IRQ` reading `0xFFFFFFFF`. Now `irq |= value & 0xFF`, in the reference and the C++.
- **Fixed: a write to a read-only register was logged as "unimplemented".** FSTAT, FLEVEL, RXF0-3, DBG_PADOUT, DBG_PADOE, DBG_CFGINFO, INTR and IRQ0/1_INTS are RO; the other blocks ignore such a write silently (SYSINFO, TBMAN, PSM, RESETS, the SSI's TXFLR, WATCHDOG's REASON), and these now do too. (`SMx_ADDR` already did.)
- **Checked and consistent, by probe:** the reset values (FSTAT `0x0F000F00`, DBG_CFGINFO `0x00200404`, everything else 0; INTR reads `0xF0` at reset because the four TXNFULL bits are levels of an empty FIFO - the table's "0x0" is a placeholder for a status register), FSTAT/FLEVEL following the FIFOs, `FDEBUG.TXOVER` on a write to a full TX FIFO without touching the FIFO, `RXUNDER` on a read of an empty RX FIFO (reads 0), the write-1-to-clear behaviour of FDEBUG and IRQ, IRQ_FORCE setting the flags the state machines see, INTR's SM-IRQ bits 11:8 following IRQ 3:0, `INTS = (INTR & INTE) | INTF`, `SMx_INSTR` reading the instruction at the PC and executing a write, `SMx_ADDR` being 5 bits and read only, `INSTR_MEMx` write-only (a read warns, as before: the datasheet does not say what a read returns).
- **Not independently sourced / unchanged:** a read of `IRQ_FORCE` (WF) and `TXFx` return what they did; `DBG_PADOUT`/`DBG_PADOE` follow the model's pin images (30 bits).
- Tests: `tests/test_pio_datasheet.py` (one case through the bus, both builds), `tests/cpp/test_pio.cpp`, and the oracle's stimulus now writes the reserved IRQ_FORCE bits and the read-only registers.

### SYSINFO and TBMAN (datasheet 2.20 and 2.22; ported as they were read, 2026-10-06)

Both are read-only identity blocks; read together with pico-sdk's `platform.c`/`platform.h`, `hardware/regs/sysinfo.h` and `tbman.h` (fetched from raw.githubusercontent.com).

- **CHIP_ID revision followed nothing (fixed).** The reference returned a constant `0x10002927` (revision 1). pico-sdk's `rp2040_chip_version()` documents the revision as "1 for B0/B1, 2 for B2" and `rp2040_rom_version()` the ROM's version byte (address `0x13`) as "1 for B0, 2 for B1, 3 for B2" - so a chip booted with `--bootrom b2` must read revision 2. The revision now follows the loaded bootrom's version byte (`0x20002927` for B2, `0x10002927` otherwise, including a chip with no bootrom loaded). `platform.c` also pins `MANUFACTURER_RPI == 0x927` and `PART_RP2 == 0x2`, which are what the register carried; the register's "reset" in `sysinfo.h` (`0x20002927`, manufacturer `0x926`) disagrees with the SDK's own code and is not followed. Tests: `tests/test_sysinfo_tbman.py` (both builds); oracle mutants `revision_always_2`, `revision_2_from_b1`, `revision_ignores_rom`, `rom_version_wrong_byte`, `manufacturer_926`, `part_wrong`.
- **Writes to a read-only register were logged as "unimplemented" (fixed).** SYSINFO's three registers and TBMAN's PLATFORM are read-only; a write has no effect and now logs nothing (an offset that is no register still does). Mutants `write_warns`, `chip_id_write_warns`.
- **Left, unsourced:** SYSINFO `PLATFORM` reads `0x2` (ASIC) - the datasheet's table gives both fields reset `0x0` and its overview says the register "will always read as 1"; the value is rp2040js's ("verified against the silicon") and is kept. `GITREF_RP2040` reads `0xE0C912E8`, likewise from rp2040js. TBMAN `PLATFORM` reads `1` (ASIC, per the field table); `tbman.h`'s reset `0x5` contradicts its own reserved bits and is not followed. The tool's one remaining note on these blocks is the PLATFORM placeholder above.

### BUSCTRL, SYSCFG and VREG_AND_CHIP_RESET (datasheet 2.1.5, 2.21 and 2.10.6; ported as they were read, 2026-10-07)

Read with `scripts/audit/datasheet_conformance.py` (first pass: BUSCTRL 2 findings, SYSCFG 6, VREG_AND_CHIP_RESET 3; after the fixes: only the two placeholder notes below) and the register tables and chapters of the datasheet (`RP-008371-DS-1`).

- **BUSCTRL lacked `BUS_PRIORITY` (fixed).** Offset 0, table 5: PROC0 bit 0, PROC1 bit 4, DMA_R bit 8, DMA_W bit 12, all RW, reset 0; it warned "unimplemented" on both directions. It is stored with mask `0x1111`. `BUS_PRIORITY_ACK` (RO) now ignores a write instead of falling to the unimplemented path; it reads 1 ("goes to 1 once all arbiters have registered the new priority levels ... almost immediately") although the table's reset is 0 - the model has no arbiters to wait for. The performance counters never count (no bus fabric is modelled), as before.
- **SYSCFG implemented one register of seven (fixed).** The six others warned. They are stored with the datasheet's reserved bits and resets: `PROC1_NMI_MASK` 32 bits, `PROC_CONFIG` (DAP instance ids 31:24 RW, reset `0x10000000`; `PROC0/1_HALTED` RO, read 0), `PROC_IN_SYNC_BYPASS` (30 bits), `..._HI` (6 bits), `DBGFORCE` (RW bits `0xEE`, reset `0x66`, the two `SWDO` bits RO, read 0), `MEMPOWERDOWN` (8 bits). They change nothing (one core, no debug port, no memories to power down). SYSCFG now has state, so it resets through `RESETS` bit 18 (`RESET_SYSCFG`, datasheet 2.14.3, new constant) and `RP2040.reset()` calls it; `PROC0_NMI_MASK` is the C++ `Cpu`'s own field.
- **VREG and BOD wrote every bit (fixed).** `VREG` keeps VSEL 7:4, HIZ 1 and EN 0 (`0xF3`), `BOD` VSEL 7:4 and EN 0 (`0xF1`); ROK (VREG bit 12, RO) and the reserved bits cannot be set. `CHIP_RESET` was right.
- **Left, unsourced:** `VREG.ROK` reads 0 (the table's reset). Section 2.10.4 describes it qualitatively (high once the output reaches the assertion threshold, low again in high-impedance mode or after a step to a higher voltage); there is no regulator model to derive it from and no firmware here reads it. `PROC_CONFIG.PROC0/1_HALTED` reads 0 for the same reason. `BUS_PRIORITY_ACK` reading 1 and `CHIP_RESET` resetting to `HAD_POR` (the table says 0, a fresh emulated chip is a power-on) are the tool's two remaining notes.
- Tests: `tests/test_busctrl_syscfg_vreg_datasheet.py` (both builds), the shared oracle `tests/utils/small_blocks_diff.py` + `tests/test_small_blocks_diff.py` (34 logic mutants), `tests/cpp/test_busctrl.cpp`, `test_vreg.cpp`, `test_syscfg.cpp`.

### PSM, XOSC and RESETS (datasheet 2.13.5, 2.16.7 and 2.14.3; ported as they were read, 2026-10-07)

Read with `scripts/audit/datasheet_conformance.py` (first pass: PSM 1 finding, XOSC 2, RESETS 2; after the fixes only the notes below) and the chapters 2.13, 2.14 and 2.16 of the datasheet (`RP-008371-DS-1`).

- **PSM matched the tables** (FRCE_ON, FRCE_OFF, WDSEL: bits 16:0, reset 0; DONE read-only). Its one change: a write to DONE is ignored instead of logged as "unimplemented" (the same fix as SYSINFO/TBMAN). `DONE = (MASK & ~FRCE_OFF) | (FRCE_ON & FRCE_OFF)` - FRCE_ON wins over FRCE_OFF - is the model's reading of "indicates the peripheral's registers are ready to access"; the datasheet does not say what happens when both are set. Not independently sourced.
- **XOSC kept reserved bits and wrong reset values (fixed).** `CTRL` stored bits 31:24 (reserved); it keeps 23:0, and FREQ_RANGE (11:0) is fixed at `0xAA0` ("resets to 0xAA0 and cannot be changed"): another value sets `STATUS.BADWRITE` ("an invalid value has been written to CTRL_ENABLE or CTRL_FREQ_RANGE or DORMANT") and warns. `STARTUP` resets to `0xC4` (table 262, "approx 50 000 cycles"; it was 0). `DORMANT` resets to WAKE (`0x77616B65`, table 261: "On power-up this field is initialised to WAKE") and "an invalid write will also select WAKE": the register reads WAKE, the oscillator wakes, and BADWRITE is set (it stored the written value). Mutants for each.
- **RESETS.RESET_DONE was a constant (fixed).** The datasheet (2.14.2): "a bit for each peripheral, that gets set once the peripheral is out of reset"; it is now `~RESET & 0x1FFFFFF` (the model's peripherals come out of reset at once). A write to it is ignored silently.
- **Left, unsourced or by decision:**
  - `RESETS.RESET` resets to 0, the datasheet's `0x1FFFFFF` ("every peripheral ... is held in reset at power-up", 2.14.1). The model does not gate a peripheral on its RESET bit, and the bootrom and the SDK's un-reset leave everything firmware uses out of reset; starting with all bits set would only change what `RESET_DONE` reads before firmware touches it. The tool's one remaining finding on RESETS, kept on purpose.
  - XOSC `CTRL.ENABLE` with a value that is neither `0xD1E` nor `0xFAB`: the datasheet says "an invalid setting will enable the oscillator"; the reference sets BADWRITE and warns without changing the state, and a power-up `ENABLE` of 0 (which is neither code) cannot enable it either. The two statements are not reconciled by the datasheet, so the reference's behaviour is kept. Whether CTRL reads `ENABLE = DISABLE` (`0xD1E`) after power-up ("initialised to DISABLE") is likewise unsourced: it reads 0.
  - XOSC `COUNT` does not count down (a write stores 8 bits, a read returns them): the countdown ("writing a non-zero value starts it, it stops at zero") is a feature, not implemented. A firmware that polls COUNT for zero after writing it would hang; none of the firmware here does. XOSC startup is instant (STABLE is set when ENABLE is written), the STARTUP delay is not timed.
- Tests: `tests/test_psm_xosc_resets_datasheet.py` (both builds), the shared oracle `tests/utils/psm_xosc_resets_diff.py` + `tests/test_psm_xosc_resets_diff.py` (38 logic mutants of the reference), `tests/cpp/test_psm.cpp`, `test_resets.cpp`, `test_xosc.cpp`.

### WATCHDOG (datasheet 4.7 and errata RP2040-E1; ported as it was read, 2026-10-08)

Read with `scripts/audit/datasheet_conformance.py` (first pass: 2 findings, LOAD "unimplemented" and TICK "RW bits that read 0") and the chapter 4.7 (overview, tick generation, the counter, scratch registers, the SDK driver and `hello_watchdog`), the register tables 546-550, and the errata.

- **`TICK.CYCLES` was ignored (fixed).** TICK: CYCLES (8:0, RW, reset 0) is stored and read back, ENABLE (9, reset 1) as before, RUNNING (10, RO) reads 1 while the generator runs, COUNT (19:11, RO) reads 0 (no model behind it, not independently sourced).
- **A write to REASON was logged as "unimplemented" (fixed).** REASON is read-only (table 548); the write is ignored, silently, as for the other read-only registers. `CTRL.ENABLE` (RW) reads as written; it used to read the countdown's run state.
- **Checked and found as the datasheet says:** CTRL (ENABLE reset 0, the three PAUSE bits reset 1, TRIGGER self-clearing and never read back, TIME = the counter), LOAD (24 bits), the eight scratch registers, REASON (both bits clear after a hardware reset), the errata RP2040-E1 (the countdown runs at twice the tick: `watchdog_enable()`'s `delay_ms * 1000 * 2` relies on it), and the reset-cause bookkeeping of 0089 (a watchdog reset does not reset the block; the RUN-pin/power-on one does).
- **The tick is the chip's timer reference (implemented, 2026-10-08; the "TIMER ignores the tick" item).** Sources: datasheet 4.7.2 ("this tick is also distributed to the timer (see Section 4.6) and used as the timer reference"), 4.6.4 ("The Watchdog tick must be running for the timer to start counting. The SDK starts this tick as part of the platform initialisation code."), 2.4.5.1.1 (SysTick "uses a 1us pulse as a clock enable. This is generated in the watchdog block as timer_tick"), and pico-sdk `hardware_ticks/ticks.c` (`tick_start()`: "On RP2040, this also provides a tick reference to the timer and SysTick"; the cycles go to `watchdog_hw->tick` with ENABLE) and `pico_runtime_init/runtime_init_clocks.c` (`start_all_ticks()` runs **after** every clock is configured, `cycles = clock_get_hz(clk_ref) / MHZ`). The model: `tick_hz = clk_ref / CYCLES` while TICK.ENABLE is set, 0 otherwise (`tick_frequency()` in `_watchdog.py`). The chip tells the watchdog `clk_ref` whenever the CLOCKS block changes it (`update_clocks()`), the watchdog tells the TIMER (`tick_changed(hz)`) and SysTick, and its own countdown runs at `2 * tick_hz`:
  - the TIMER counts once per tick, so at the nominal tick (crystal 12 MHz / 12) one count is a microsecond, exactly as before; a tick from the ring oscillator (6.5 MHz nominal) is 541.7 kHz, and a stopped tick freezes the count and the armed alarms exactly as PAUSE does. The count and the alarms stay continuous across a change of rate (the alarms are re-timed); a chip reset restarts the count from zero and keeps the tick, which is not one of the TIMER's registers;
  - SysTick with CLKSOURCE = 0 counts on the tick (stopped when the tick is, CSR.ENABLE still reads as written); CLKSOURCE = 1 does not care;
  - the watchdog's own reset (RUN-pin/power-on, `reset()`: "reset by rst_n_run", 4.7.1) now covers CTRL and TICK too: after it the tick is not running until the firmware starts it again. A watchdog reset does not touch the block, as before.
  - **Consequence to know:** a bare chip's TIMER does not count (TIMERAWL stays 0) until something has written TICK with CYCLES != 0; the unit tests that read it before any firmware use `tests/utils/tick.py::start_tick()`, which does what `clocks_init` does. Every firmware family tested here starts the tick through the SDK.
  - **Not independently sourced:** CYCLES = 0 is taken as "not running" (the datasheet does not say what a zero count does; it is the reset value, and the TIMER must not count out of reset, 4.6.4). The ring oscillator is its nominal 6.5 MHz (`RP2040.rosc_freq`; real silicon varies with PVT and the ROSC registers, which are not modelled). The crystal is always oscillating once clk_ref selects it (the XOSC's startup is instant, see above).
  - Tests: `tests/test_tick.py` (end to end, both builds), `tests/test_timer_parity.py` (random tick changes), `tests/test_ppb_diff.py` and `tests/test_watchdog_diff.py` (random clk_ref and TICK writes, the followers compared), C++ checks in `test_timer.cpp`, `test_ppb.cpp` and `test_watchdog.cpp`.
- **Left, unsourced:** `LOAD` is write-only (type WF, a write FIFO register); a read warns "unimplemented" and reads `0xFFFFFFFF`, because the datasheet does not say what a read returns. The pause bits (debug mode, JTAG) are stored and do nothing: there is no debugger to pause for.
- Tests: `tests/test_watchdog_datasheet.py` (both builds), the oracle `tests/utils/watchdog_diff.py` + `tests/test_watchdog_diff.py` (37 logic mutants of the reference; simulated time, the timeout firing, a failing reset handler, clk_ref and TICK changes and what follows them), `tests/cpp/test_watchdog.cpp`.

### RTC (datasheet 4.8, CLOCKS 2.15 for clk_rtc, pico-sdk `hardware_rtc`; ported as it was read, 2026-10-08)

Read with `scripts/audit/datasheet_conformance.py` (first pass: 12 registers, 10 with findings - six unimplemented, SETUP_0/SETUP_1 keeping reserved bits, IRQ_SETUP_0 reading 0 after a write of ones, and CTRL **raising** `ValueError: year 0 is out of range`), the chapter 4.8 (storage format, leap year, interrupts, reference clock, the programmer's model with the SDK's `rtc_init`, `rtc_set_datetime`, `rtc_get_datetime`, `rtc_set_alarm`), the register tables 552-564, and `rtc.c` itself (the alarm handler, `rtc_enable_alarm`/`rtc_disable_alarm`). The old reference was a stub that came from rp2040js: a wall-clock `datetime` offset by simulated time, no CLKDIV_M1, no alarm, no interrupt.

- **The time base was not the chip's (fixed).** The RTC counts seconds of `clk_rtc / (CLKDIV_M1 + 1)` (4.8.4); the old block ignored CLKDIV_M1 and clk_rtc and counted real simulated seconds. Now the second is `(CLKDIV_M1 + 1) * 1e9 / clk_rtc` simulated nanoseconds, one alarm on the chip's clock, restarted by a LOAD and by a change of CLKDIV_M1 or clk_rtc, left alone by a CTRL write that only keeps the state. **clk_rtc is the CLOCKS block's** (new `RPClocks.rtc_freq`: ENABLE and not KILL, AUXSRC PLL_USB / PLL_SYS / ROSC / XOSC over `CLK_RTC_DIV`, 24.8, INT 0 = 2^16; GPIN0/1 are not modelled and give 0; tables 234/235), pushed to the RTC by `update_clocks()` and `reset_clock_tree()` the way `clk_ref` is pushed to the watchdog; a write to `CLK_RTC_CTRL`/`CLK_RTC_DIV` now calls `update_clocks()` (it did not: clk_rtc was registers only). **Consequence, the same call as for the watchdog's tick:** a bare chip's RTC does not count - and `CTRL.RTC_ACTIVE` never rises, so the SDK's `rtc_set_datetime()` would wait - until firmware has configured clk_rtc. Every firmware family tested here does it in its clock initialisation (`clocks_init`: XOSC / 256 = 46875 Hz, `rtc_init()`: CLKDIV_M1 = 46874). Unit tests use `tests/utils/tick.py::start_rtc_clock()`.
- **A calendar, not a `datetime` (fixed).** Seven plain fields as in table 551 (year 12 bits ... second 6), carrying second -> minute -> hour -> day -> month -> year; the day of the week is only incremented ("it will only increment the existing value", 4.8.1.1); a year divisible by 4 is a leap year unless `CTRL.FORCE_NOTLEAPYEAR` (4.8.2: so 2100 is a leap year unless firmware sets the bit - the datasheet NOTE says to). **The datasheet words the leap test as "the current value of YEAR in SETUP_0"**; read literally that would make every later year leap or not as the *loaded* year was, which the NOTE under it (set the bit "after 2096 Mar 1 ... before 2100 Feb 28") contradicts, so the running year is tested. Illegal values ("The RTC does not check that the programmed values are in range. Illegal values may cause unexpected behaviour", 4.8.1) no longer raise: a field past its limit wraps at its next carry (second/minute >= 59, hour >= 23, month >= 12, day >= the month's length, a month outside 1..12 has 31 days, a day of week >= 6 wraps to 0) - **the model's own rule, the datasheet gives none.**
- **`CTRL.LOAD` worked only together with ENABLE (fixed).** The SDK writes LOAD alone and ENABLE after (`rtc_set_datetime`); LOAD (type SC) now loads at once whether or not the RTC is enabled and reads back 0. `RTC_ACTIVE` (RO) reads ENABLE *and* a running clk_rtc; `FORCE_NOTLEAPYEAR` (RW) is stored.
- **`RTC_0` latches `RTC_1` (new).** "Reading RTC_0 latches the value of RTC_1" (4.8.5.3): a read of RTC_1 returns the latch; nothing is latched before the first RTC_0 read (RTC_1 reads 0) - *not independently sourced*. An alias write to RTC_0 decodes against a read, so it latches too.
- **The alarm and the interrupt (new).** `IRQ_SETUP_0/1` (masks 0x17FFFF1F and 0xF71F3F3F; `MATCH_ACTIVE` (RO) reads `MATCH_ENA`), `INTR/INTE/INTF/INTS`, NVIC interrupt 25. The raw interrupt is a **level**: `MATCH_ENA` and every enabled field equal to the counter, re-evaluated after each second, a load, a write to IRQ_SETUP_0/1, INTE or INTF and a reset; with no field enabled every second matches. Source for "level": the SDK handler's own comment, "Always disable the alarm to clear the current IRQ. ... If it matches on a second it can keep firing for that second." **Consequence to know:** a repeating alarm whose handler re-enables it inside the matching second (`rtc_enable_alarm()` in `rtc_irq_handler`) is raised again at once, until the second is over - on silicon every `MATCH_ENA` change takes about two clk_rtc periods to arrive, here none, so the emulated storm is tighter than the real one (the handler runs back to back for that one simulated second). Left as is; see the backlog.
- **Corrected after the port (2026-10-12): INTS is `(INTR & INTE) | INTF`, not `(INTR | INTF) & INTE`.** The first version of the RTC masked the force with INTE. The datasheet calls INTS "Interrupt status after masking & forcing" (table 564) - the mask first, then the force - and every other interrupt block of the chip here does it that way (ADC, PIO, CLOCKS' `INTS = (INTR & INTE) | INTF`): a forced bit reaches the status register and the NVIC line whether or not it is enabled. Found while reading the PIO's INTF/INTS for the second pass; fixed in the reference and the C++, the datasheet test and the C++ check say it now, and the oracle gained two mutants of the wrong order (`ints_masks_the_force`, `irq_masks_the_force`) - the old oracle's mutants had all been built on the same wrong formula, which is why none noticed.
- **Registers read against the tables:** CLKDIV_M1 (16 bits), SETUP_0 (YEAR 23:12, MONTH 11:8, DAY 4:0), SETUP_1 (DOTW 26:24, HOUR 20:16, MIN 13:8, SEC 5:0), CTRL, IRQ_SETUP_0/1, RTC_1/RTC_0 (RO), INTR/INTE/INTF/INTS - every reserved bit is gone on a write of ones, every reset value 0, writes to the read-only registers are ignored silently.
- **Reset** (`RESETS.RESET_RTC`; `rtc_init()` starts with it): every register 0, the counter 0, the latch 0, the divider stopped, the interrupt line down; clk_rtc is the CLOCKS block's and is kept. *The counter's reset value is "-" in tables 559/560: 0 is the model's own.*
- **Left, unsourced / not modelled:** the synchroniser latency (a write to the RTC "will take 2 clk_rtc clock periods to arrive", 4.8.4; `RTC_ACTIVE`, `MATCH_ACTIVE`, LOAD and the alarm's level are instant here); the alarm waking ROSC/XOSC from dormant mode (4.8.3, 4.8.5.5); an external GPIN reference (clk_rtc from GPIN0/1 gives 0); the divider's phase at enable (a new second begins when the RTC is enabled).
- **Checked live:** MicroPython 1.29.0 (`machine.RTC().datetime()`: set 2024-02-28 23:59:57, 4 s of `time.sleep` later 2024-02-29 00:00:01, a leap day; elapsed `ticks_ms` 4000) and CircuitPython 10.2.1 (`rtc.RTC().datetime`: 2024-12-31 23:59:57 -> 2025-01-01 00:00:02 after 5 s). MicroPython's RP2040 `machine.RTC` has no alarm (`datetime()` only), so the alarm is held by the datasheet tests, the oracle and the C++ checks, not by a live boot.
- Tests: `tests/test_rtc_datasheet.py` (13 cases, both builds), the oracle `tests/utils/rtc_diff.py` + `tests/test_rtc_diff.py` (the SDK's `rtc_set_datetime` / `rtc_set_alarm` sequences with times next to every carry, the clock tree, a failing interrupt line, ten seeds, 83 logic mutants of the reference, a control, coverage counts), `tests/cpp/test_rtc.cpp` (gcc and clang) and a textual mutation pass over `core/rtc.hpp` (224 mutants: 222 killed, one did not compile, one survivor argued equivalent - `reset()`'s `retime(true)`: the block is disabled by then, so the divider is cancelled whichever way it is asked).

### SSI, second pass: the operation chapters 4.10.1-4.10.12 (2026-10-11; before the XIP_CTRL / USB ports)

Read against the reference (`_ssi.py`, `core/ssi.hpp`) for what the model *is* - a flash-command emulation driven by the bootrom through DR0 - and for what the datasheet says that is checkable without a FIFO model:

- **Fixed: DR1..DR35 were unimplemented.** The register list has "Data Register 0 (of 36)" at 0x60 and RX_SAMPLE_DLY at 0xF0, and 4.10.12 says "Writing to any of these address locations has the same effect as pushing the data from the pwdata bus into the transmit FIFO. Reading from any of these locations has the same effect as popping data from the receive FIFO". The reference served DR0 only; a read or write of the other 35 locations (0x64..0xEC, word aligned) warned "unimplemented" and read 0xFFFFFFFF. They are the data register now, in the reference and the C++ together. The same paragraph says "sixty-four 32-bit locations" while the list and the layout say 36 (0x60 + 36 x 4 = 0xF0): **a conflict inside the datasheet**, the model follows the list. Tests: `tests/test_ssi.py::test_dr1_to_dr35_are_the_data_register_too` (both builds), a C++ check, the oracle (`dr_aliases_unimplemented` mutant, the alias offsets in the DR traffic).
- **Checked and consistent:** the IP modification "XIP accesses are byte-swapped, such that the least-addressed byte is in the least-significant position" is a little-endian read of the flash, which is what XIP reads here are (they bypass the SSI). The "instruction appended to the address" modification and the continuation-code formats change only what is clocked on the wire, which the model does not clock. TXFIFO/RXFIFO depth 16 x 32 bits matches `kRxCapacity` for the RX side. The FIFOs are cleared when SSI_EN is cleared (done in the first pass). Reads of an empty RX FIFO return 0 (4.10.7: "0s are read back from the receive FIFO").
- **Conflict inside the datasheet, left as the table has it:** 4.10.6 says CTRLR0[3:0] (DFS) "is invalid and writing to this register has no effect" (DFS_32 at 20:16 replaces it), while the CTRLR0 table lists DFS as RW. The model keeps the bits (the table), as for DMARDLR above.
- **Not independently sourced, unchanged:** BAUDR is stored whole although 4.10.4 says SCKDV holds "any even value in the range 0 to 65,534" (the text does not say bit 0 reads 0); a write to DR while SSIENR is clear is dropped (the chapter says nothing about it); SR/RISR reset values are placeholders in the table.
- **Already in the backlog, now sourced further:** the SSI's own interrupts (4.10.7: txe/txo/rxf/rxo/rxu/mst, all level, ORed after masking, **only the combined request goes to the interrupt controller - that line is XIP_IRQ, NVIC 6, "from the SSI block that makes up part of the XIP block"**, used "in a configuration where code is running from SRAM", where "the XIP block could be used as a normal SSI peripheral"), the thresholds (TXE: entries <= TXFTLR; RXF: entries >= RXFTLR + 1) and the transfer modes (TMOD: TX only does not fill the RX FIFO, RX only needs CTRLR1.NDF, EEPROM read). All of it belongs to the "SSI used as an ordinary SPI master from RAM" feature, which this model does not have; nothing in the XIP boot path uses it.
- **Not read yet:** the XIP chapter 2.6 (the XIP cache, the streaming FIFO and its DREQ, `XIP_CTRL`'s registers) - that is the audit of the XIP_CTRL port.

### PPB (SysTick, NVIC, SCB)

Audited before this record was opened, as part of the C++ port, and written up in the [0096](0096-cpp-mcu-core.md) progress log (the entry "The PPB is C++"): four reference quirks fixed against the datasheet and the M0PLUS tables (`SYST_RVR` is 24 bits, the SysTick period is RELOAD + 1 with the first period after a CVR write, `CLKSOURCE` = 0 runs from the 1 MHz reference clock, and the fourth listed there), with pico-sdk's `m0plus.h`. The ARM Architecture Reference Manual page was not readable (JavaScript-rendered), so the parts of SysTick that only the ARM manual defines stay marked as the model's own: the step "the next tick after a CVR write loads RELOAD" and the reset values (the datasheet says UNKNOWN; the model resets RVR and the counter to 0xFFFFFF).

## Second pass: the operation chapters of the closed blocks

Read after the register tables, 2026-10-06 (SPI 4.4.3.7-4.4.3.16, I2C 4.3.5-4.3.10 and 4.3.16, DMA 2.5.1-2.5.5, CLOCKS 2.15.3, PIO 3.5.6). Corrections made, in the reference and the C++ together, each with a test:

- **I2C: a write to `IC_DATA_CMD` while the block is disabled is lost** ("If the IC_DATA_CMD register is written before the DW_apb_i2c is enabled, the data and commands are lost as the buffers are kept cleared", 4.3.10.2.1). The reference queued it. pico-sdk enables before it writes commands.
- **DMA: `CHAN_ABORT` clears the transfer counter** ("This clears the transfer counter and forces the channel into an inactive state", 2.5.5.3); the reference cleared BUSY and kept TRANS_COUNT. (RP2040-E13 - an abort of a running channel may raise a completion IRQ - is an erratum of the silicon and is not modelled.)
- **PIO: OUT / SET / side-set pin mappings wrap after GPIO31** ("this mapping continues for PINCTRL_OUT_COUNT bits, wrapping after GPIO31", 3.5.6); the reference shifted the data and dropped what left the word (a "TODO: wrapping after pin 31" in both implementations). They rotate now.

Read and found consistent, or left as what the model does not do:

- **SPI** (frame formats): the wire-level behaviour - clock and frame-select timing, the Texas Instruments and Microwire formats (Microwire is half-duplex with an 8-bit control word), slave select and the pad-enable lines - is below the model's word-level `on_transmit`; nothing in it contradicts the block, and the formats are in the "left" list. Words go out most-significant bit first and the receive side right-justifies, as modelled.
- **I2C**: the master's TX FIFO management (no STOP unless IC_DATA_CMD.STOP is set, SCL held low on an empty FIFO, RESTART by bit) matches; **not modelled**: with `IC_RESTART_EN` = 0 a RESTART must become a STOP followed by a START, and with it set a change of direction between two queued commands issues a RESTART by itself ("If the direction of this transfer differs from the previous transfer, the combined format is used", 4.3.5.2) - the block restarts only on the RESTART bit, which is what pico-sdk sets; disabling is instantaneous (the datasheet lets a master disable only after a command with STOP, and has IC_ENABLE_STATUS report it); table 451 confirms TX_EMPTY and RX_FULL are levels "set and cleared by hardware", the other interrupts are cleared by software (TX_EMPTY is still raised by a command being taken, not as a pure level).
- **DMA**: the credit-based DREQ scheme (a counter per channel, one request in flight per credit) is modelled as a level (transfer while the DREQ is up), the transfer FIFOs and bus arbitration are not; TRANS_COUNT reload and the "trigger by TRANS_COUNT uses the value just written" rules, the alignment caution and the chain and null-trigger rules match.
- **CLOCKS**: glitchless mux and SELECTED, the aux mux, KILL and ENABLE (only clk_peri's is modelled), the divider range (1 or 2.0 to 2^24-0.01, jittery) and the duty-cycle correction are hardware detail the model collapses to "the clock has this frequency now"; the frequency counter (2.15.4) and resus (2.15.5) remain in the backlog.
- **PIO** (3.5.6): the output priority between state machines (the highest-numbered wins per GPIO, separately for levels and directions) comes out right because the machines are stepped in order within a cycle and a later write wins; a side-set overlapping an OUT/SET of the same cycle takes precedence (it is applied after); the 2-cycle register stage and the 2-flip-flop input synchroniser (4 cycles with `INPUT_SYNC_BYPASS` clear) are **not modelled** - a state machine sees its own outputs and the inputs at once.
- **SSI** (4.10.3, 4.10.5): **disabling the SSI (SSIENR = 0) clears the FIFOs** ("The transmit and receive FIFO buffers are cleared when the DW_apb_ssi is disabled", 4.10.5) - the model's RX queue survived; now cleared (a correction, with a test). Read and left: the three IP modifications of 4.10.3 (XIP accesses byte-swapped, the XIP instruction appended after the address when `SPI_CTRLR0.INST_L` is 0, DMARDLR); the FIFOs are 16 entries of 32 bits and the thresholds work as in the tables (TX: at or below, RX: at or above threshold + 1) - none of it is modelled, the SSI being the bootrom's flash-command path. **A conflict inside the datasheet:** 4.10.3 says "The reset value of DMARDLR is increased from 0 to 4", while the register table (and pico-sdk's `SSI_DMARDLR_RESET`) say 0; the model follows the table and the SDK.

## Not audited yet

Everything ported on the 0096 branch has a section above. What has **not** been read against the datasheet:

- **Blocks without a C++ port yet** are not a separate "to audit later" class: the rule (the maintainer's, 2026-10-06, restated 2026-10-08) is that **a block that is ported is audited and ported in one pass, by the whole recipe of [core-block-recipe.md](../reference/core-block-recipe.md)** - section A (datasheet and pico-sdk, with `scripts/audit/datasheet_conformance.py`), the reference fixed in its own commit, the oracle, the C++ block, the checks and the mutation pass, and the record - while the blocks that were ported *before* this rule got their audit after the fact, which is what the sections above are. So what is not read against the datasheet is exactly what is not ported: USB (the controller and the CDC host), ROSC (today an `UnimplementedPeripheral` and a `rosc_freq` attribute; the clock tree and, since the RTC, `clk_rtc` read that constant), IO_BANK0 and PADS_BANK0 (`peripherals/io.py`, `pads.py`; with the shared pin banks of #29) and XIP_CTRL (`xip_ctrl.py`), and CLOCKS / PLL, which are Python on purpose (0096, "Clocks done") but are owed to the core by the gate of 0096 and will then go through the same recipe. WATCHDOG, RTC, SYSINFO, TBMAN, BUSCTRL, SYSCFG, VREG_AND_CHIP_RESET, PSM, XOSC and RESETS are done, see above. The tool's register-level pass can be pointed at any of them (`BASES` in the script lists their addresses) but it has not been run on them for this record.
- **Parts of the audited blocks still skimmed or skipped**: the SSI's XIP streaming (it is read together with the XIP_CTRL port, datasheet 2.6). The SSI's own FIFO and interrupt behaviour, the I2C slave-mode and bus-clear chapters and the CLOCKS resus chapter are read, and what they require is written down in "Requirements of the features the model does not have" below (2026-10-12); the DMA example chapters are tests ("DMA second pass: the example chapters"), the sniffer is in the backlog. The operation chapters of the rest are in the second pass above.
- **What the datasheet does not say**, so no source exists to check against: the quotient and remainder of a signed divide by zero, what a read of a SIO `SET`/`CLR`/`XOR` alias returns, the PPB's reset values.

## Open features found by the audit (backlog)

> The living table of what is implemented, what is a stub and what is missing, with *when* each item is to be done, is [docs/reference/feature-coverage.md](../reference/feature-coverage.md); the table below is the audit's own record of what it found missing.

None of these is a correction of the reference; each is missing behaviour, left for its own decision (CLAUDE.md: documenting is not implementing).

| Block | Missing | Why it matters |
|---|---|---|
| PIO | `EXECCTRL_OUT_STICKY`, `INLINE_OUT_EN`, `OUT_EN_SEL` (the existing TODO) | programs that rely on a sticky OUT or an inline output enable |
| PIO | autopull at the OUT that reaches the threshold and between OUTs (3.5.4.2); today it happens when the next OUT starts | TX FIFO levels and DREQ timing read slightly high |
| DMA | the sniffer (`SNIFF_CTRL`, `SNIFF_DATA`, `CTRL.SNIFF_EN`), `HIGH_PRIORITY` and arbitration, the error flags, the `DBG_CTDREQ` counter | CRC over a transfer; contention timing |
| DMA | I2C (and SSI) DREQs: the I2C block has no DREQ output | DMA-driven I2C |
| RTC | the clk_rtc synchroniser latency (`RTC_ACTIVE`, `MATCH_ACTIVE`, LOAD arrive after 2 clk_rtc periods), the dormant-mode wake of ROSC/XOSC, an external GPIN reference | a repeating alarm's re-enable inside its matching second storms tighter than on silicon; `rtc_run_from_external_source()` gets no clock |
| CLOCKS | `ENABLED0/1`, the resus, the clock gating of WAKE_EN/SLEEP_EN (the frequency counter `FC0_*` is done, see CLOCKS second pass) | the resus needs a stopped-clock model |
| SIO | the inter-core FIFO and core 1 (record 0053) | `_thread`, multicore |
| I2C | slave mode, the general call and START byte, `TX_EMPTY_CTRL`, `RX_FIFO_FULL_HLD_CTRL`, the "FIFOs stay flushed until IC_CLR_TX_ABRT" rule | slave devices |
| SSI / SPI | the SSI's own FIFOs, interrupts and DMA; SPI slave mode, the receive timeout, TI and Microwire frames | XIP streaming, SPI slaves |

## What was checked against pico-sdk rather than the datasheet

Each of these is a statement about what real firmware does, read from the SDK source (`raspberrypi/pico-sdk`, master) and not from memory: `spi_init` sets both SSPDMACR enables then SSE (`hardware_spi/spi.c`); `uart_init` writes DMACR with both enables unless `PICO_UART_NO_DMACR_ENABLE` (`hardware_uart/uart.c`); `i2c_init`, `i2c_set_baudrate` and `i2c_write_blocking` clear `enable` before writing `con`, `tar` and the counts (`hardware_i2c/i2c.c`); `SIO_INTERP0_ACCUM0_ADD_BITS` is 0x00ffffff and `interp_add_accumulator` stores a `uint32_t` (`hardware_regs/sio.h`, `hardware_interp/interp.h`); `dma_channel_abort` writes `abort` and polls the channel's `CTRL_TRIG.BUSY`, **not** `CHAN_ABORT` (`hardware_dma/dma.h`) - the datasheet's instruction to poll CHAN_ABORT is what justifies its read value, not the SDK. Anything in the sections above that cites the SDK without being in this list was cited from the datasheet's embedded SDK excerpts.

## Progress log

- 2026-10-06: opened. Tool and the first-pass register-level findings above; nothing fixed yet.
- 2026-10-06: **TIMER read against the datasheet and fixed** (PAUSE, TIMEHW/TIMELW, DBGPAUSE, the 64-bit wrap and the C++ int64 undefined behaviour it exposed). Next: ADC.
- 2026-10-06: **ADC read against the datasheet and fixed** (8-entry FIFO, READY needs EN, DIV 24 bits). Next: PWM.
- 2026-10-06: **PWM read against the datasheet and fixed** - the phase-correct output and period (the 0096 "bug 4"), CSR/DIV reserved bits, PH_ADV at full speed. Next: UART.
- 2026-10-06: **UART read against the datasheet and fixed (reserved bits, IFLS/ILPR/DMACR/RSR/ECR, DREQ rule, TXE/RXE/UARTEN gating, overrun)**; reference, C++, oracle and tests together.
- 2026-10-06: **SPI read against the datasheet and fixed** (reserved bits, nothing sent while SSE = 0, the DMA requests gated by SSE and SSPDMACR, loop back, SSPRIS reset). Next: I2C.
- 2026-10-06: **I2C read against the datasheet's register tables and fixed** (FIRST_DATA_BYTE bit, configuration writable only while disabled, widths and minima, stored registers, RX_FULL as a level, abort flushes RX, ACTIVITY). Next: SIO, DMA, SSI, CLOCKS/PLL, PIO - then the small blocks as they are ported.
- 2026-10-06: **SIO and DMA read against the datasheet and fixed** (SIO: QSPI GPIO width, divider reset, ACCUM_ADD width, FORCE_MSB per lane; DMA: pacing timer period, TIMER3, reads of CHAN_ABORT/MULTI_CHAN_TRIGGER/FIFO_LEVELS). Next: SSI, CLOCKS/PLL, PIO.
- 2026-10-06: **SSI read against the datasheet's register tables and fixed** (reserved bits, SPI_CTRLR0 reset, TXFLR read only, the unimplemented registers stored). Next: CLOCKS/PLL, PIO.
- 2026-10-06: **CLOCKS and PLL read against the datasheet's register tables and fixed** (DIV masks, RESUS_CTRL, WAKE/SLEEP/INT registers stored, PLL widths). Next: PIO.
- 2026-10-06: **PIO read against the datasheet (instruction set, shift counters, the SMx register tables) and fixed** (IFFULL/IFEMPTY without auto-transfer, PULL behind an autopull, OUT 32, the reset OSR count, SM_RESTART and EXEC_STALLED, reserved bits). Open: FJOIN, OUT_STICKY. **Every block ported on this branch has now been read.**
- 2026-10-06: **SYSINFO and TBMAN ported to C++ and read against the datasheet and pico-sdk** - the CHIP_ID revision now follows the bootrom (B2 reads 2), writes to the read-only registers are silent. One shared lockstep oracle (`tests/utils/ident_diff.py`), C++ checks, native shells. Next: SYSCFG, BUSCTRL, VREG, then PSM/XOSC/RESETS, then WATCHDOG/RTC.
- 2026-10-07: **BUSCTRL, SYSCFG and VREG_AND_CHIP_RESET read against the datasheet and ported to C++** - `BUS_PRIORITY` added, SYSCFG's register file and `RESET_SYSCFG`, VREG/BOD write masks. One shared oracle (`tests/utils/small_blocks_diff.py`). Next: PSM, XOSC, RESETS, then WATCHDOG, RTC.
- 2026-10-07: **PSM, XOSC and RESETS read against the datasheet and ported to C++** - XOSC CTRL/STARTUP/DORMANT fixed, RESETS.RESET_DONE follows RESET, writes to the DONE registers silent. One shared oracle (`tests/utils/psm_xosc_resets_diff.py`). Next: WATCHDOG, RTC.
- 2026-10-08: **WATCHDOG read against the datasheet and ported to C++** - `TICK.CYCLES` stored, a write to REASON silent. **The watchdog's tick is now the TIMER's and SysTick's reference** (clk_ref / CYCLES; the TIMER does not count until firmware starts it), sourced to datasheet 4.6.4/4.7.2 and pico-sdk's `ticks.c`/`runtime_init_clocks.c`. Next: RTC.
- 2026-10-08: **RTC read against the datasheet and ported to C++** - the time base is `clk_rtc / (CLKDIV_M1 + 1)` (clk_rtc is now the CLOCKS block's: a bare chip's RTC does not count until firmware configures it), a real calendar with leap years and `FORCE_NOTLEAPYEAR`, LOAD without ENABLE, the RTC_0 -> RTC_1 latch, the match alarm and its interrupt (NVIC 25). One oracle (`tests/utils/rtc_diff.py`). Next: USB, then the pure-Python blocks nobody has read yet (ROSC, IO_BANK0, PADS_BANK0, XIP_CTRL).
- 2026-10-12: **UART second pass (operation chapters 4.2.2-4.2.6) read**: no code change; receive-timeout interrupt, per-entry DR error flags and the TX interrupt level rule recorded as gaps. Next: ADC.
- 2026-10-12: **ADC second pass (operation chapter 4.9.2) read and fixed**: a FIFO read now lowers the IRQ line, clearing EN halts a free-running capture; reference, C++, oracle mutants and tests together. This closes the item-1 readings of "Not audited yet". Next: ROSC port (0096).

## MicroPython's own test suite against the emulator (first run, 2026-10-12, v1.29.0, native build)

MicroPython's `tests/run-tests.py -t <pty>` drives the emulated board through the console's `--pty` (the bridge of `docs/reference/mpremote.md`; `pyboard.py` opens the device with `serial.Serial`, so a `socket://` URL does not work there but a pty does). `unittest` is installed on the board with `mpremote mip install unittest` (host-side download). Run on `ports/rp2/*.py` and `extmod/machine_*.py`: **11 pass, 3 skip, 2 fail, 1 test file with one failing case** (before the transmitter got a time; the two UART failures then passed) (`rp2_dma.py`: 8 of 9 cases pass).

- `rp2_dma.py::test_time_taken_for_large_memory_copy` expects 30-80 us (`ticks_us`) for a 16 KiB copy; the DMA here finished in about 1 ms - not too fast, too slow: every transfer was an alarm at +0 ns, so it ran between two CPU instructions of the polling loop, one word per loop turn. **Fixed 2026-10-12** (below).
- `machine_uart_tx.py`, `machine_uart_irq_txidle.py` (**both pass since the transmitter got a time**): both measured the time a `UART.write` takes at 2400/9600/115200 baud and the moment `IRQ_TXIDLE` fires. The transmitter here is instant (see "UART" above: "no timing of transmission"), so the timing windows are missed. Same root as the receive-timeout and TX-level gaps of the UART second pass: **a baud-rate time base for the UART** would close all three.
- Skipped by the files themselves (`machine_pinbase`, `machine_pulse`, `machine_signal`: they need wiring or a feature check that does not apply).
- `rp2_thread_reset_part*` and the `.exp`-comparing multi-file tests were not run in this first pass.

`.github/workflows/ci-micropython-official.yml` (with `tests/micropython_official/run_suite.sh`) repeats this on every push and pull request as a **report only** job: `continue-on-error`, the script always exits 0, the table goes to the job summary and an artifact. It does not gate anything.

### The interpreter tests on the emulated Cortex-M0+ (first scan, 2026-10-12, v1.29.0, native build)

The same bridge, one file at a time (45 s limit, the emulator restarted after a hang), on the directories that exercise the *interpreter* rather than a peripheral - a test of the CPU model from the outside, by thousands of Python and Thumb-assembly programs the C++ core was never written against:

| directory | files | pass | skip | fail / hang |
|---|---|---|---|---|
| `float` | 69 | 64 | 5 (double precision, `divmod` ... : the port is single-precision) | 0 |
| `basics` | 576 | 543 | 31 (features this build leaves out: `fun_code`, `exception_chain`, ...) | 0 (2 "ERROR": `unittest` was not on the board; the CI script installs it) |
| `inlineasm/thumb` | 19 | 8 | 11 (instructions of the M4/VFP the M0+ lacks) | 0 |

**No failure and no hang in 664 files.** The `rv32` and `xtensa` directories of `inlineasm` are for other chips and were not run. (The first report of this scan said `inlineasm` was done before it had been scanned: the glob missed the `thumb/` subdirectory. Corrected here.) Not yet scanned: `extmod` beyond `machine_*`, `micropython`, `import`, `misc`, `io`, `thread` (needs core 1), `stress`, `cmdline`.

`.github/workflows/ci-micropython-official.yml` now runs four groups as separate report-only jobs: the peripherals (`ports/rp2`, `extmod/machine_*`), `float` + `inlineasm/thumb`, `basics` a-l and `basics` m-z. The run summary lists only the files that did not pass, with the counts.

### DMA second pass: the example chapters (2.5.6), 2026-10-12

Read: 2.5.6.1 (reconfigure a channel from its completion interrupt, `dma/channel_irq`) and 2.5.6.2 (control blocks, `dma/control_blocks`). Both are now **tests that run the examples as the datasheet writes them** (`tests/test_dma_datasheet_examples.py`, both builds): the register values the SDK's helpers produce for the two channels, the real UART as the paced peripheral.

- **`control_blocks`:** a control channel (32-bit, both addresses increment, a write ring of `1 << 3` bytes, count 2) loads seven `{length, address}` pairs into the data channel's **alias 3** (`TRANS_COUNT`, then the `READ_ADDR` trigger), the data channel (8-bit, UART TX paced, `CHAIN_TO` the control channel, `IRQ_QUIET`) chains back, and the null block ends the list. Result: `Transferring one word at a time.\n` out of the UART, in order, and the data channel's raw interrupt is raised **only** for the null trigger at the end (`INTR` is exactly that channel's bit). Consistent with the datasheet; no change to the DMA.
- **`channel_irq`:** a paced channel writes the same word three times (read address fixed), raises IRQ 0 at the end of the block, and the handler (`ints0` clear, then `READ_ADDR_TRIG`) gives it the next word - four blocks, `AAABBBCCCDDD`, the NVIC line up at each completion. (The example paces with a PIO TX FIFO; the UART's stands in, the DREQ mechanism being the same.)
- **The sniffer** (the CRC section, 2.5.5.2) stays in the backlog; the decision recorded in the DMA sniffer note above is unchanged.

### Cortex-M0+ system control and the MPU (2.4.4, 2.4.6; done 2026-10-12)

The last registers the conformance tool listed as absent from the processor's own bus: `AIRCR`, `SCR`, `CCR`, `SHCSR` and the five MPU registers, plus three registers that stored reserved bits (`VTOR[7:0]`, `SHPR2`, `SHPR3`). Done in the reference, the C++ block and the shell together, with the oracle and the C++ checks (`tests/test_ppb.py`, `tests/test_ppb_diff.py`, `tests/cpp/test_ppb.cpp`):

- **`AIRCR`** reads `0xFA050000` (`VECTKEYSTAT`); a write is taken only with `VECTKEY` `0x05FA` (all sixteen bits; the read-back key `0xFA05` is not the write key). `SYSRESETREQ` (bit 2) calls the new host hook `on_system_reset_request` (C++: `PpbHost::system_reset_request`), whose default is **a core-only reset** - core reset, PPB reset, jump to the flash entry point - because the datasheet (2.4.2.9) says the signal resets "the core only", unlike a watchdog reset. A real chip-level reset is one hook assignment away for a host that wants the board to restart. `VECTCLRACTIVE` (a debug bit) is ignored.
- **`SCR`** stores `SLEEPONEXIT`, `SLEEPDEEP`, `SEVONPEND` (mask `0x16`) and nothing acts on them (a **stub**: `WFI` ignores `SLEEPONEXIT`, `WFE` ignores `SEVONPEND`; listed in `feature-coverage.md` and `register_status.py`). **`CCR`** reads `0x208` (`STKALIGN`, `UNALIGN_TRP`), writes are ignored. **`SHCSR`** has the one field of the M0+, `SVCALLPENDED` (bit 15), which is the core's `pending_svcall`.
- **MPU**: `MPU_TYPE` `0x800` (eight unified regions), `MPU_CTRL` (3 bits), `MPU_RNR` (4 bits), `MPU_RBAR` (`ADDR` 31:8; a write with `VALID` set selects the region from `REGION`; `REGION` reads back `RNR[3:0]`; `VALID` reads 0), `MPU_RASR` (mask `0x1707FF3F`), one pair per region 0-7; region numbers 8-15 read 0 and drop writes. **Nothing is enforced** (a stub): there is no HardFault on a privilege or a region violation in the core. Reset clears `SCR` and the whole MPU.
- **Reserved bits** that used to stick: `VTOR` keeps 31:8, `SHPR2` keeps `PRI_11` (31:30), `SHPR3` keeps `PRI_15`/`PRI_14` (31:30, 23:22).
- Verification: 40 new logic mutants of the reference (`MUTATIONS`), all caught; the C++ mutation pass over the new header lines leaves only the unobserved `return true` of the window path (the bus ignores a write's result) and a pre-existing `ISRPENDING` `||` (covered by the oracle); the conformance tool's remaining notes for these registers are artefacts of its probing (it leaves `MPU_RNR` at 15 after writing ones, where `RBAR`/`RASR` read 0, and `AIRCR` reads `VECTKEYSTAT` where the datasheet's reset column says 0).

### SPI transmitter time (4.4.3; done 2026-10-12, first of the two backlog features the maintainer released)

A word written to `SSPDR` used to be on the wire for no time: the device answered from inside `on_transmit` and the RX FIFO filled at once, so `BSY` was never seen high by a polling driver and MicroPython's `machine_spi_rate` (which times a transfer) failed. Now, in the reference, the C++ block (`SpiHost::bit_ns`, a frame `Alarm` on the chip's clock) and the shell together:

- A word occupies the wire for `DSS + 1` periods of `SSPCLKOUT` (`clk_peri / (CPSDVSR x (1 + SCR))`, 4.4.3.6) from the moment it leaves the TX FIFO; `BSY` is up for that long and the next word starts when the previous one completes.
- **The device API is unchanged.** `on_transmit(value)` still runs when the word leaves the FIFO, and the device answers with `complete_transmit(rx)` as before; a reply that arrives *before* the frame ends is held until it does, one that arrives *after* completes at once (a slow device such as the ST7735S or the e-paper). Loopback is the same, and a stray completion with nothing in flight is still accepted at once.
- No frame time for a slave (`MS`; the clock comes from outside and slave mode is not modelled) or while `CPSDVSR` is 0 (no bit clock configured).
- **Not modelled, by the "never guess" rule:** the gap between words in the `SPH = 0` formats, where `SSPFSSOUT` is pulsed high between words (4.4.3.10: "must be pulsed HIGH between each data word transfer") - the datasheet gives its length only in a figure whose text could not be read - and the abort of a word in flight when `SSE` is cleared. Both are listed in `feature-coverage.md`.
- **Result in CI** (MicroPython official tests, run 38271569292, commit 146be9b): the peripherals group went from 2 failures to 1 - `machine_spi_rate.py` passes now; only `ports/rp2/rp2_dma.py` (the DMA copy-time case) fails, plus the 6 skips.
- The native SPI now needs the native `SimulationClock` (as the UART and ADC do); the oracle (`tests/utils/spi_diff.py`) gained a `tick` op, the frame state and the clock's alarms in its snapshot, and ten mutants of the frame logic, all caught.

### PIO FIFO joining (3.5.3, SHIFTCTRL.FJOIN_TX / FJOIN_RX; done 2026-10-12, second of the two backlog features the maintainer released)

The bits were stored and ignored, so MicroPython's `PIO.JOIN_TX` / `JOIN_RX` and every program that deepens one FIFO behaved as if it had four entries. Now, in the reference (`_state_machine.py`), the C++ machine (`PioFifo` got a runtime `size` and `resize()`) and the shell together:

- `FJOIN_TX`: the TX FIFO is 8 deep and the RX FIFO is "disabled as a result (always reads as both full and empty)"; `FJOIN_RX` the mirror image; both bits make both unavailable. An unavailable FIFO is a FIFO of size 0, so everything else follows from the existing logic rather than from special cases: a `PUSH` / `PULL` that needs it stalls (and `NOBLOCK` drops), a TXF write is `TXOVER`, an RXF read is `RXUNDER`, `FSTAT` shows both flags, the DREQ is down, `TXNFULL` / `RXNEMPTY` are 0, and `FLEVEL` reads up to 8 for the deep one.
- **"FIFOs are flushed when this bit is changed"**: a write that changes either FJOIN bit replaces both FIFOs with new empty ones, republishes the two DREQs, lets a stalled `PUSH` / `PULL` re-check its FIFO (a machine stalled on RX that is joined to RX goes on) and re-announces the interrupts. A write that leaves the bits as they were (AUTOPUSH changing, say) flushes nothing. A reset puts both back to four.
- Verification: the PIO oracle now drives joins (random configuration writes and a scenario that joins, fills past four entries from both sides with `PUSH NOBLOCK` through `SMx_INSTR` and TXF writes, and drains), with coverage counts for each join kind and for FIFOs holding more than four; the C++ checks (`tests/cpp/test_pio.cpp`) and the chip-level `tests/test_pio_fjoin.py` run on both builds. The PIO oracle has no logic mutants; the C++ mutation pass over the new lines leaves only pre-existing unrelated survivors.

### DMA transfer rate (2.5; done 2026-10-12)

"The DMA can perform one read access and one write access, up to 32 bits in size, every clock cycle" (2.5, and "1 word per clock" for DREQ credit, 2.5.3.2): a transfer that is ready now takes **one `clk_sys` cycle**, not zero. A transfer used to be scheduled `+0 ns`, which the clock runs between two CPU instructions, so a CPU polling `BUSY` completed one word per loop turn - about 1 ms for 16 KiB where the silicon needs 4096 cycles (33 us at 125 MHz) and MicroPython's `rp2_dma` asserts 30-79 us. Now `schedule_transfer()` uses `1e9 / clk_sys` (reference) and `DmaBlock::transfer_ns` (C++; the shell keeps it current through a `clk_sys` listener, so a transfer never calls Python); a pacing timer or a waiting DREQ is unchanged. Contention for the bus with the cores or other channels, which would make it slower, is not modelled (listed in `feature-coverage.md`). The DMA oracle's `clk` op now announces the change to the clock listeners like `_set_clk_sys` does; `tests/test_dma_time.py` and the C++ checks pin the cycle.

### ROSC - audited and ported in one pass (2.17; 2026-10-12, the first of the ports after the 0098 backlog)

The first port done by the recipe's order (datasheet first, then the oracle, then C++): the chip had an `UnimplementedPeripheral` at `0x40060000`, so every access warned and read `0xFFFFFFFF`. Sources: datasheet 2.17 and tables 264-273, pico-sdk `hardware/regs/rosc.h` (resets and enumerations) and `pico_rand/rand.c` (RANDOMBIT is what the SDK's entropy code reads and asserts `STATUS.ENABLED` for).

- **Registers, as the tables lay them out:** `CTRL` (ENABLE 23:12 `0xD1E` disable / `0xFAB` enable; FREQ_RANGE 11:0, reset `0xAA0`; reads `0x00FABAA0` at power-on: "this field is initialised to ENABLE"), `FREQA` / `FREQB` (PASSWD `0x9696`, four 3-bit drive strengths), `DORMANT` (`0x636F6D61` / `0x77616B65`), `DIV` (`0xAA0 + div`: 0 divides by 32, 1-31 by div, anything else div = 31; resets to div 16), `PHASE` (PASSWD `0xAA`, reset `0x8`), `STATUS` (STABLE, `BADWRITE` write-1-to-clear, DIV_RUNNING, ENABLED - `0x80011000` once running: the table's "resets to 0 but transitions to 1 during chip startup"), `RANDOMBIT`, `COUNT`. `BADWRITE` is set - and the write warned - for every invalid write the table lists (ENABLE, FREQ_RANGE, FREQA, FREQB, DIV, PHASE, DORMANT).
- **The frequency (2.17.1-2.17.4):** `rp2040.rosc_freq` follows the block: 6.5 MHz nominal at div 16 / LOW, scaled by 16 / divisor and by the range's ratio (MEDIUM 1.33, HIGH 2, TOOHIGH 4: "approximately", from 2.17.3), and the clock tree is told (`update_clocks()`), so a firmware that runs from the ROSC and raises its speed gets it. **The drive strengths (FREQA / FREQB) have no effect:** the datasheet only says they raise the frequency "not linearly", with no figures.
- **`RANDOMBIT` (2.17.5):** a hook, `random_bit`, whose default is a seeded generator (the same firmware sees the same bits on every run, so a simulation is reproducible - replace the hook for real entropy). While the oscillator is disabled the register holds its last value and the hook is not asked.
- **`COUNT` (2.17.6):** an 8-bit down counter that counts at the ROSC frequency while the oscillator is enabled and stops at zero; derived from the chip's clock when read (no alarm), folded in before the frequency or ENABLE changes.
- **Reset:** `PSM.WDSEL`'s ROSC bit gates it exactly like XOSC: a power-on / RUN-pin reset restores the block, `watchdog_reboot()` ("everything apart from ROSC and XOSC") does not.
- **Not independently sourced, kept as the model's choice (said in the reference's docstring):** that the stored registers read back what was written (an invalid FREQ_RANGE leaves the previous range, an invalid DIV reads back as div = 31, a wrong FREQA / FREQB password stores drive strengths of 0, a wrong PHASE password stores ENABLE = 1 / SHIFT = 0), `DIV_RUNNING` following ENABLED, RANDOMBIT holding while disabled, `0xAA0` (the reset value, not an enumerated range) being LOW, and COUNT counting at the *output* frequency (the datasheet says "the ROSC frequency").
- **Not modelled:** the oscillator really stopping (DISABLE and DORMANT are stored; the datasheet says the chip would lock up if it was the system clock, and DORMANT needs the chip-wide dormant model of 2.11.3), STABLE as a start-up time (it follows ENABLE; "responds immediately" and "starts immediately" per 2.17.2), the phase-shifted output.
- **Verification:** the oracle (`tests/utils/rosc_diff.py`) drives every register through the bus and its aliases with the datasheet's interesting values, time, resets, the clock tree (`rosc_freq`, `clk_sys`, every `update_clocks()` call logged) and the random source (every call logged); 70 logic mutants of the reference are all caught (two equivalent ones were removed: the redundant `CTRL_MASK`, and an unmasked random bit until the hook returned more than one bit); the C++ checks and the mutation pass over `core/rosc.hpp` leave three equivalent survivors (the deleted copy-assignment, the `count_epoch` reset which is overwritten by the next COUNT write, and `>=` against `>` where both give 0). `tests/test_rosc.py` runs on both builds. The conformance tool's remaining notes are artefacts: a write of all ones is an invalid write for every password-protected register, and RANDOMBIT is a random bit where the table's reset column says 1.

### A limit found on the way: the clock's `double` nanoseconds (2026-10-12; **fix authorised - its own record, [0100](0100-integer-clock.md)**)

MicroPython's `rp2_dma.py::test_time_taken_for_large_memory_copy` still fails in the *full* official-suite sequence although it passes alone, in pairs with each earlier file, and on a fresh emulator - and the DMA transfer rate itself is right (one word per `clk_sys` cycle, 32.8 us for 16 KiB). The cause is the simulated time: after the `extmod/machine_*` files `clock.nanos` is about `7.8e16` (2.5 years of simulated time; an earlier file leaves a very long idle jump behind). A `double` of that size has a resolution of 16 ns, so `now + 8 ns == now` and every DMA transfer lands "at the same instant" (a diagnostic print showed `now == last` on consecutive transfers). It is not a DMA bug: **any delay shorter than the resolution is lost once the clock passes 2^53 ns (about 104 days)** - UART and SPI frame times, PWM, PIO pacing alike.
- **The fix, decided with the maintainer on 2026-10-12 and moved to [0100](0100-integer-clock.md) (order: after the XIP_CTRL port, before USB - the first two ports have almost no timers):** an integer time base (int64 ticks, in the C++ `Clock` and the Python clocks) instead of `double` nanoseconds; the fractional delays (a UART bit time, a pacing period) are converted at the edge. It is a cross-block change: the clock, every alarm user and the oracles' time snapshots.
- **Until then:** `rp2_dma.py` is expected to fail in the full CI sequence; the cheaper alternative - finding which file leaves the long idle jump and bounding it - treats the symptom only.

### IO_BANK0, IO_QSPI, PADS_BANK0, PADS_QSPI - audited (2.19; 2026-10-12; phase 0 of the pin-bank port)

The audit that opens the port of the IO / PADS windows and the shared pin bank (#29 of 0096). Sources: datasheet 2.19 (function select, interrupts, pads) and tables 283-344, pico-sdk `hardware/regs/io_bank0.h` and `pads_bank0.h` (resets). The conformance tool learned the four blocks' base addresses. Fixed in the reference and in the C++ pin (`core/pin.hpp`) together; the Python windows (`RPIO`, `RPPADS`) serve both builds.

- **`PADS_BANK0.GPIOn` reset was `0x36`, the datasheet and the SDK say `0x56`:** the model had the input buffer *off* and 12 mA drive (the value rp2040js carries); the pad resets with IE on, 4 mA, pull-down, Schmitt. Bits 31:8 are reserved and no longer stick.
- **`SWCLK` (`0x7C`, reset `0xDA`) and `SWD` (`0x80`, reset `0x5A`) are registers now** (stored; the debug pads have no pin behind them).
- **`PADS_QSPI` listed its pads in the wrong order:** the datasheet's table 344 is SCLK, SD0, SD1, SD2, SD3, SS (SS at `0x18`), while `RP2040.qspi` and IO_QSPI are SCLK, SS, SD0..SD3 - so a PADS_QSPI access at `0x08` reached the SS pin. The window now maps by the datasheet's order (the reset values of record 0050 were right, their addresses were not).
- **`GPIOn_CTRL` kept reserved bits**: it keeps only IRQOVER, INOVER, OEOVER, OUTOVER and FUNCSEL (`0x3003331F`).
- **INTR's level bits were not level bits.** They were latched at the last input *change*, so they were wrong from boot (a chip reads `0x11111111` in `INTR0` after reset: every pin low behind its pull-down, with the input enabled) and after a pad write. They now follow the level the interrupt logic sees - the pad's reading behind its input buffer - at all times, and the **edge bits latch on a transition** of that level: `set_input_value()` of the level the pin already had raised an edge before, and so did a reset. A write of the pad register (IE, a pull on a floating pin) is an input change when it moves that level.
- **IO_QSPI had IO_BANK0's register layout:** its interrupt registers sit right after the six pins (`INTR` `0x30`, `PROC0_INTE/INTF/INTS` `0x34-0x3C`, proc 1 `0x40-0x48`, dormant wake `0x4C-0x54`), one word each; the window used `0xF0...`. `RPIO` now takes the layout from the bank.
- **`PROC1_*` and `DORMANT_WAKE_*` registers exist** (24 words in IO_BANK0, 6 in IO_QSPI): enable / force are stored per pin, status is `(INTR & INTE) | INTF`. They raise no line - there is no core 1 and no dormant mode to wake (2.11.5.2, which the model lacks); the requirement is already in the "features without a model" list.
- **A QSPI pin driven by SIO took its level from the wrong register:** with `FUNCSEL` = SIO it used `GPIO_OUT` / `GPIO_OE` bit *n* (GPIO *n*'s) instead of `GPIO_HI_OUT` / `GPIO_HI_OE` ("the QSPI IOs are controlled via the SIO_GPIO_HI_x registers, mapped in the order SCK, CSn, SD0, SD1, SD2, SD3", table 282); the parked fix of 0096 (2026-10-05) is made, as the maintainer decided on 2026-10-12 ("by the datasheet"). The XIP function keeps the pin output-enabled with level 0, as before.
- **Not modelled, said once:** pad drive strength, slew rate, Schmitt trigger and `VOLTAGE_SELECT` (stored only); a frequency or timing at the pad; the function table's peripherals other than SIO, PWM and the two PIOs (UART, SPI, I2C, clock in / out, USB: the pin does not connect them, the peripherals reach the outside through their own callbacks); the 2-/4-cycle latency from output to input (3.5.6.1).
- **Tests:** `tests/test_io_pads_datasheet.py` (10 cases on both builds), `tests/cpp/test_pin.cpp` (the inline transcription of the reference updated to the new rules). The pin layer has no lockstep oracle yet: it is built with the next phase (the shared bank), where the C++ blocks are checked against this reference.

**Ported (phases 1-3, 2026-10-12).** The oracle, the shared bank and the C++ windows followed the same day:
- **`tests/utils/pin_diff.py` / `tests/test_pin_diff.py`** compare two chips fed one stream (register reads and writes of the four blocks with the atomic aliases, input drive / release / refresh, SIO / PWM / PIO source changes, pin and window resets): one with the pure-Python pins and windows (`_gpio_pin.py`, `_io.py`, `_pads.py`), one with what the facades give. Everything observable is compared after each step - every listener call with its states, every `update_io_interrupt()`, the PWM input hook, the logger, the pins' fields, the register files and the stored proc 1 / dormant masks. Self-proof as for every oracle: damage to the candidate is caught at the step it is done, and 90 source-text mutants (50 of the pin, 40 of the windows) are all caught; coverage counts keep the runs from being green by doing nothing.
- **`core/io.hpp` and `core/pads.hpp`** are the windows, `native/_io.pyx` / `_pads.pyx` their shells; both work on the `PinBank` the chip's pins share. A pin object is a view of one bank slot. SIO, PIO and PWM registers are bound as direct pin sources; the pin tells its listeners and the IO interrupt line through the bank's host.
- **C++ checks:** `tests/cpp/test_io_pads.cpp`, with the textual mutation pass over `io.hpp` / `pads.hpp` (105 and 68 mutants; the survivors are equivalent for the two bank sizes that exist - the word-count rounding for 30 and 6 pins, the reset repeated in `init`, the deleted copy operators, the sentinel-less group loop - or compile-time only).
- **Kept as it was, said once:** a pin's first announcement comes from the state it was built in (INPUT, a quirk of `GPIOPin.__init__` that the Cython and C++ keep); an input change on QSPI pin *n* reaches GPIO *n*'s PWM / PIO reactions.
- **Replacing a block re-points the pins by itself:** the direct sources are bound at construction, and `RP2040.sio`, `RP2040.pwm`, `RP2040.pio` (and an element assigned in that list) are properties / a list that bind again, so a chip whose block was swapped (the oracles do) never reads the old one. `tests/test_pin_bank.py` pins it.

**Second pass: SIO to pin, and the pad's output path (2026-10-12; asked for after the port, "is the bank audited?").** Read: 2.3.1.2 (SIO GPIO registers, tables 18-31), 2.19.2 (function select, tables 279-282), the pad registers (OD / IE), 2.21 (SYSCFG `PROC_IN_SYNC_BYPASS`). The SIO registers themselves match (reset 0, reserved bits, read-back of the last write, SET / CLR / XOR aliases, `GPIO_HI_*` in the order SCK, CSn, SD0..SD3). What differed:
- **Fixed - a QSPI pin driven by SIO never told its listeners.** `GPIO_HI_OUT` / `GPIO_HI_OE` changed the level (the phase 0 fix made the level right) but only the 30 GPIO pins were re-evaluated after an SIO write, so a listener on a QSPI pin (an external device, the SSI's chip select) heard nothing until some other write touched the pin. `SioHost::update_pins` now carries the bank (`qspi`); the reference does the same loop; a C++ check, the SIO parity test (its stand-in chip has QSPI pins now) and `test_io_pads_datasheet.py` pin it. A cross-block change on the SIO / pin boundary, small and in the direction the datasheet requires.
- **Fixed - `PADS.OD` was ignored.** "Output disable. Has priority over output enable from peripherals": a pad with OD set is not driven, whatever the function, the output enable and the overrides say (its state is the pull). `STATUS.OETOPAD` is the IO block's side and does not include OD. Reference, C++ (`state_code`), the oracle (a mutant), `test_pin.cpp` and a datasheet test.
- **Not changed, decision needed - a pin the chip drives reads back as 0.** The datasheet says of the SIO function (and of the PIO's): "the input is always connected, so software can check the state of GPIOs at any time". The model's input is the external world only (`set_input_value`, else the pull), never the pad's own driven level, so after `GPIO_OUT_SET` on an output pin `GPIO_IN` still reads 0, `INTR` sees no edge from the chip's own output change, a PIO `wait` or `in pins` on an output pin sees 0 (MicroPython's `Pin.value()` on an `OUT` pin goes through `gpio_get`). Proposed: when the pad is driven by the chip (`output_enable` and not `OD`) the level it presents is the driven level; an external drive set at the same time is a contention the model has to pick a winner for (the external one, as a stronger source, is the natural choice, and `ExternalDevice`s that read the pin then still see the chip's level through `value`). A behaviour change of the pin layer's core, with a large test surface: not made without a go-ahead.
- **Said once, not modelled:** the input synchronisers (two clk_sys cycles of latency between the pad and `GPIO_IN`, bypassed per pin by `SYSCFG.PROC_IN_SYNC_BYPASS` / `_HI`, which are stored; the PIO's `INPUT_SYNC_BYPASS` is the same kind of stub).

### Requirements of the features the model does not have (read 2026-10-12; **nothing implemented**)

The chapters below describe behaviour the emulator has no model for, so there was no code to compare with. Reading them produced the list of what each would have to do; each is a backlog feature (CLAUDE.md: documenting is not implementing) and starts only on an explicit go-ahead.

**SSI, outside XIP (4.10.5-4.10.8).** Today the SSI is a flash-command model with no FIFOs of its own: `SR` always reads TFE and TFNF (RFNE from a byte queue), `ISR` and `RISR` read 0, `IMR`, `TXFTLR`, `RXFTLR`, the DMA registers are stored and ignored. What the datasheet requires of the real thing:
- **Two 16-entry FIFOs of 32-bit entries**, one frame per entry (right-justified), both **cleared when `SSI_EN` is 0** and at reset (this one is modelled: disabling clears the RX queue).
- **Five level interrupts, all combined into one request** (`ISR = RISR & IMR`; only the combined line reaches the NVIC): `txe` while the TX FIFO holds `<= TXFTLR` entries (cleared by hardware when a write takes it above); `txo` when an APB write meets a full TX FIFO (the data is discarded; sticky until `TXOICR` is read); `rxf` while the RX FIFO holds `>= RXFTLR + 1` entries (`RFT` 0 means one or more, 15 means 16); `rxo` when a received frame meets a full RX FIFO (discarded; sticky until `RXOICR`); `rxu` when `DR` is read empty (reads 0; sticky until `RXUICR`); `mst` (multi-master contention, master configuration only; until `MSTICR`).
- **Transfer modes (`CTRLR0.TMOD`)**: 00 transmit and receive; 01 transmit only (received frames are not stored); 10 receive only (the slave never pops the TX FIFO); 11 EEPROM read (master only, not in SSP mode: transmit the opcode and address frames, capture nothing until the TX FIFO is empty, then receive `CTRLR1.NDF + 1` frames; more TX frames than needed lose read data).
- **32-bit frames**: `CTRLR0[20:16]` (`DFS_32`) holds the frame size minus one, 0 to 31; `CTRLR0[3:0]` is invalid and writing it has no effect.
- What would use it: XIP streaming (with the XIP_CTRL port), bit-level flash drivers that watch `SR`/`TXFLR`/`RXFLR` (the bootrom's `flash_put_get` loop works today through the byte queue), DMA to and from the SSI (a DREQ is not routed: backlog row "DMA | I2C (and SSI) DREQs").

**I2C slave mode (4.3.10.1; the master is the only mode modelled).**
- **Configuration**: `IC_ENABLE.ENABLE` 0; `IC_SAR` (9:0); `IC_CON` with `IC_SLAVE_DISABLE` 0 and `MASTER_MODE` 0 (slave-only, never master and slave together: the combination SLAVE_DISABLE 0 + MASTER_MODE 1 is the one to avoid), `IC_10BITADDR_SLAVE` for 7/10-bit matching; enable. The reset values are stored today; nothing acts on them.
- **Slave-transmitter**: a matching address with the read bit raises `RD_REQ` (bit 5 of `IC_RAW_INTR_STAT`) and **holds SCL low until software answers**; stale TX FIFO data at that moment raises `TX_ABRT` (`ABRT_SLVFLUSH_TXFIFO`) and the FIFO is flushed and stays frozen until `IC_CLR_TX_ABRT` is read; software writes `IC_DATA_CMD` (bit 8 = 0) and clears `RD_REQ`/`TX_ABRT`; the byte goes out. **Bulk**: while the master ACKs and the FIFO has data the slave keeps sending without another `RD_REQ`; with the FIFO empty after an ACK it holds SCL and raises `RD_REQ` again; a master NACK ends the transfer, clears the leftover FIFO data and raises `TX_ABRT`. Not applicable in Ultra-Fast mode (no reads).
- **Slave-receiver**: the byte goes to the RX FIFO; with the RX FIFO full the slave holds SCL low until there is room; `RX_FULL` follows `IC_RX_TL`; software reads `IC_DATA_CMD` (7:0).
- **Related registers**: `RX_DONE` (bit 7), `IC_SLV_DATA_NACK_ONLY`, `IC_ACK_GENERAL_CALL` and the general-call and START-byte handling, `IC_STATUS.SLV_ACTIVITY`, the slave's `START_DET`/`STOP_DET`/`ACTIVITY`.
- **A bus model is the real prerequisite.** A slave needs an *external master* driving the bus (the ExternalDevice side); today the emulated I2C is the master and the devices it addresses are the slaves (`external/`), so slave mode would need the two roles to be swappable, not just the register behaviour.
- **Bus clear (4.3.13) has no registers on this chip**: the chapter describes the feature of the IP (nine SCL pulses to free a stuck SDA, then STOP; a stuck SCL is recoverable only by a hardware reset), but the RP2040 register map has no `SDA_STUCK` register, enable bit or interrupt bit, so there is nothing for firmware to program or read: **not applicable, nothing to model**. Likewise 4.3.12 (fast mode plus) is configuration of registers that are already stored (`IC_FS_*CNT`, `IC_FS_SPKLEN`, `IC_SDA_SETUP`).

**CLOCKS: resus and the clock enables (2.15.3.5, 2.15.5, 2.15.6.5).**
- **Resus**: counts clk_sys edges over a timeout interval measured in clk_ref cycles (`CLK_SYS_RESUS_CTRL.TIMEOUT`, which "must be >= 2x clk_ref_freq/min_clk_tst_freq") when `ENABLE` is set; with no edge it forces clk_sys to clk_ref, sets `CLK_SYS_RESUS_STATUS.RESUSSED` (RO) and the `CLK_SYS_RESUS` interrupt (`INTR` bit; `INTE`/`INTF`/`INTS` as every block), which is `CLOCKS_DEFAULT_IRQ` at the NVIC; the firmware reconfigures clk_sys and writes `CTRL.CLEAR` (bit 16, "for clearing the resus after the fault that triggered it has been corrected"). `CTRL.FRCE` (bit 12) forces a resus "for test purposes only". pico-sdk's `clocks_enable_resus` writes `INTE` = the resus bit, then `CTRL = ENABLE | 6` and installs `clocks_irq_handler`, which calls the user callback and writes `CLEAR`.
- **What the model lacks**: clk_sys here is a number that is never "stopped" (a frequency of 0 keeps the last value, see `update_clocks`), so the detection has nothing to detect. **A part is possible without it**: `FRCE` alone (switch clk_sys to clk_ref, set `RESUSSED` and the interrupt, `CLEAR` undoes it) needs no stopped-clock model and would let a firmware's resus handler be exercised; the datasheet calls resus "a debugging aid" and warns it can glitch clk_sys, and "there is no way for resus to revive the chip if clk_ref is also stopped". Decision for the maintainer: the `FRCE` slice now, or the whole with a stopped-clock model later.
- **Clock enables (2.15.3.5)**: `WAKE_EN0/1` and `SLEEP_EN0/1` (reset all ones) gate each destination clock; the cores have none, `clk_sys_busfabric` cannot be disabled in wake mode and `clk_sys_clocks` has no wake-mode enable, the gpclks have no enables. **System sleep** is entered automatically when both cores are asleep and the DMA has no outstanding transaction: the enables switch from `WAKE_EN` to `SLEEP_EN`. Stored today and not acted on; acting on them needs the sleep state of the cores (WFI/WFE) to be known to the clock block, and `ENABLED0/1` (the status of the gating) with it.
- 2026-10-12: **Requirements recorded for the features the model does not have** (SSI FIFOs/interrupts/transfer modes outside XIP, I2C slave mode, bus clear, CLOCKS resus and clock enables): read against the datasheet, written down, nothing implemented; bus clear turned out to have no registers on this chip. This closes item 1 of "Not audited yet" except the SSI XIP streaming (with the XIP_CTRL port). Next: ROSC (0096).
- 2026-10-12: **Cortex-M0+ system control and MPU registers** (`AIRCR` with the `SYSRESETREQ` hook, `SCR`, `CCR`, `SHCSR.SVCALLPENDED`, `MPU_*`; reserved bits of `VTOR`/`SHPR2`/`SHPR3`): see the section above. `SCR` and the MPU are stubs, listed as such.
- 2026-10-12: **SPI transmitter time** (a word's `DSS + 1` bit clocks on the wire, the device's reply held to the frame's end; the `SPH = 0` inter-frame gap not modelled): see the section above.
- 2026-10-12: **PIO FIFO joining** (`FJOIN_TX` / `FJOIN_RX`): see the section above. `OUT_STICKY`, `INLINE_OUT_EN`, `OUT_EN_SEL` are still open.
- 2026-10-12: **DMA transfer rate**: one `clk_sys` cycle per ready word; `rp2_dma` timing case.
**PIO pin-write arbitration: `OUT_STICKY`, `INLINE_OUT_EN`, `OUT_EN_SEL` (3.5.6.1, EXECCTRL; read 2026-10-12, nothing implemented).**
- **What the datasheet says:** each state machine may assert an OUT/SET and a side-set on every cycle, generating a 32-bit write data and write mask for the GPIO output levels and for the output enables; for each GPIO the PIO collates the writes of all four machines and applies the one from the **highest-numbered** machine, separately for levels and directions; "if no state machine asserts a write to a GPIO's level or direction, the value does not change". Side-set wins over an OUT or SET to the same pin on the same cycle.
- **`OUT_STICKY`:** the machine keeps asserting its most recent OUT/SET write on every following cycle, so it keeps overriding lower-numbered machines (and is overridden by higher-numbered ones only on the cycles they write). `SM_RESTART` clears "any pin write left asserted due to OUT_STICKY". **`INLINE_OUT_EN`:** one bit of the OUT data (`OUT_EN_SEL`) is used as an auxiliary write enable; with `OUT_STICKY`, a write whose enable is 0 deasserts the latest pin write ("masking/override behaviour").
- **What the model lacks:** one shared pin image that every machine writes at the moment its instruction runs (last write wins, no cycle in which writes are collated), and machines that run at their own divided clocks. A faithful model needs a per-cycle write stage between the machines and the pad (and the 2-cycle / 4-cycle output-to-input latency the datasheet gives), which is the **output multiplexer of IO_BANK0** (function select, `OUTOVER`/`OEOVER`) - the block of the next port.
- **When:** together with the IO_BANK0 + PADS_BANK0 port (the arbitration belongs on that side of the boundary), not before it. Not to be guessed meanwhile.

**Where each open backlog feature goes in the order (decided 2026-10-12, a recommendation the maintainer has not yet released for implementation):**
- *With a port:* the pin-write arbitration above with **IO_BANK0 + PADS_BANK0**; the SSI FIFOs, interrupts and transfer modes with **XIP_CTRL** (the XIP streaming uses the SSI, and datasheet 2.6 is read for that port).
- *After the gate review, and not in the first single-core release:* the I2C slave (needs an external-master bus model on the ExternalDevice side, a design of its own), CLOCKS resus (needs a "stopped clk_sys" model; a hardware-failure path), and the second SIO FIFO / core 1 (record 0096's dual-core seams).
- Why: a feature added to a block already in C++ is done three times (reference, C++, shell) plus the oracle; one added while its neighbour block is being ported costs no more than the port.
- 2026-10-12: **ROSC audited and ported** (registers, frequency, RANDOMBIT, COUNT, PSM.WDSEL reset; see the section above), and the clock's `double` nanosecond resolution found as the reason `rp2_dma` still fails in the full sequence (fix authorised, next).
- 2026-10-12: **IO_BANK0 / IO_QSPI / PADS audited** (the pad reset, the level / edge bits, the IO_QSPI layout, the PADS_QSPI order, SWCLK / SWD, PROC1 / DORMANT_WAKE registers, the QSPI SIO path): see the section above; phase 0 of the pin-bank port.
