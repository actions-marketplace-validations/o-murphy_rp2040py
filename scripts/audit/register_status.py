"""What the model does with each register, beyond "it answers". `register_coverage.py` turns this into four states:

    Acts       the block was read against the datasheet and ported/audited (AUDITED), and the register is neither a stub nor partial below
    Stub       STUBS: the register exists and reads back (or a constant) but nothing acts on it
    Partial    PARTIAL: it acts, except for the fields named
    Unchecked  the register answers but its block was never audited (UNAUDITED): nobody has checked that it does anything
    Absent     the register does not answer

Every STUBS / PARTIAL entry carries its evidence (the code or record that says so). **This file is a curated list, not a measurement**: a stub or a partial register that is not listed here
shows as Acts in an audited block until someone finds it - extend it whenever a "stored, not acted on" register is added, or one is found (the maintainer's rule: the checklist must not
overstate). Offsets are the datasheet's, by block.
"""

import os

os.environ["RP2040PY_SKIP_CYTHON"] = "1"

from rp2040py.peripherals import _i2c, clocks
from rp2040py.peripherals import _ssi as ssi

ALL = "all"  # every register of the block

# Blocks read against the datasheet (record 0098) and, but for the exceptions listed below, acting on their registers
AUDITED = {
    "M0PLUS", "SIO", "TIMER", "WATCHDOG", "RTC", "UART", "SPI", "I2C", "ADC", "PWM", "DMA", "PIO", "SSI", "CLOCKS", "PLL", "XOSC", "PSM", "RESETS", "SYSCFG", "SYSINFO", "TBMAN",
    "VREG_AND_CHIP_RESET", "BUSCTRL",
}  # fmt: skip
# Blocks never audited: whatever they answer is unchecked (IO_BANK0, PADS_*, USB, XIP: the ports still to do; ROSC and IO_QSPI: nothing there)
UNAUDITED = {"IO_BANK0", "IO_QSPI", "PADS_BANK0", "PADS_QSPI", "USB", "ROSC"}

# block -> {offset: what is not acted on} for registers that act but not in every field (from 0098's backlog and the second passes)
PARTIAL: dict[str, dict[int, str]] = {
    "UART": {
        0x000: "DR[11:8] (OE/BE/PE/FE of the entry) always 0",
        0x018: "FR modem bits (CTS/DSR/DCD/RI) read 0",
        0x030: "CTSEN/RTSEN flow control, SIREN/SIRLP, the modem outputs do nothing",
        0x034: "RXIFLSEL is stored; the RX interrupt comes with every byte",
        0x038: "the modem, RT and BE/PE/FE interrupts are never raised",
        0x03C: "the modem, RT and BE/PE/FE interrupts are never raised",
        0x040: "the modem, RT and BE/PE/FE interrupts are never raised",
    },
    "SPI": {
        0x000: "FRF: TI and Microwire frames are not modelled",
        0x004: "MS: slave mode is not modelled; no transmitter time",
    },
    "ADC": {0x000: "TS_EN does not gate channel 4; the SAR error flag is never raised"},
    "DMA": {
        0x00C: "CH0_CTRL_TRIG: HIGH_PRIORITY and SNIFF_EN are stored; READ_ERROR/WRITE_ERROR/AHB_ERROR are never set"
    },
    "PIO": {
        0x0CC: "SM0_EXECCTRL: OUT_STICKY, INLINE_OUT_EN, OUT_EN_SEL are stored and ignored",
        0x0D0: "SM0_SHIFTCTRL: FJOIN_TX/FJOIN_RX are stored and ignored (no 8-deep joined FIFO)",
        0x038: "INPUT_SYNC_BYPASS has no effect",
    },
    "I2C": {0x000: "IC_CON: the slave bits (IC_SLAVE_DISABLE, IC_10BITADDR_SLAVE) do nothing: no slave mode"},
    "PLL": {0x000: "CS.LOCK is always 1 (no start-up time)"},
    "SIO": {0x000: "CPUID is always 0 (no core 1)"},
    "XOSC": {
        0x008: "DORMANT is stored; the oscillator does not go dormant",
        0x00C: "STARTUP delay is stored; STABLE is set at once, not after the delay",
    },
    "WATCHDOG": {0x000: "CTRL: PAUSE_DBG0, PAUSE_DBG1, PAUSE_JTAG are stored; there is no debug halt to pause on"},
}
# block -> (offsets that are stubs | ALL, why)
STUBS: dict[str, tuple[object, str]] = {
    "XIP": (
        ALL,
        "no cache and no streaming is modelled; the registers exist so the boot path's enable/flush/poll works (record 0052, peripherals/xip_ctrl.py)",
    ),
    "SSI": (
        set(ssi.STORED_REGISTERS) | set(ssi.ZERO_REGISTERS) | {0x00, 0x04, 0x14, 0xF0, 0xF4, 0xF8},
        "the SSI is a flash-command model: these are stored (`STORED_REGISTERS`, and CTRLR0, CTRLR1, BAUDR, RX_SAMPLE_DLY, SPI_CTRLR0, TXD_DRIVE_EDGE in their own attributes) or read 0 (`ZERO_REGISTERS`) in peripherals/_ssi.py",
    ),
    "CLOCKS": (
        set(clocks.STORED_REGISTERS)
        | {clocks.CLK_SYS_RESUS_STATUS, clocks.INTR, clocks.INTS}
        | {0x00, 0x04, 0x0C, 0x10, 0x18, 0x1C, 0x24, 0x28},
        "clock gating, the resus and its interrupt are not modelled (`STORED_REGISTERS` in peripherals/clocks.py, and the status reads constant 0); the four GPOUT clock outputs (CLK_GPOUT0-3 CTRL/DIV) are stored: nothing is driven onto a pin",
    ),
    "I2C": (
        {
            _i2c.IC_SAR,
            _i2c.IC_SDA_HOLD,
            _i2c.IC_SLV_DATA_NACK_ONLY,
            _i2c.IC_DMA_CR,
            _i2c.IC_DMA_TDLR,
            _i2c.IC_DMA_RDLR,
            _i2c.IC_SDA_SETUP,
            _i2c.IC_ACK_GENERAL_CALL,
            _i2c.IC_FS_SPKLEN,
        },
        "slave mode, the DMA interface, spike suppression and the SDA timing are not modelled: stored with the datasheet's reset values (peripherals/_i2c.py)",
    ),
    "SYSCFG": (
        {0x04, 0x08, 0x0C, 0x10, 0x14, 0x18},
        "only PROC0_NMI_MASK has an effect (peripherals/_syscfg.py); the rest is stored: no second core, no debug port, no memory to power down",
    ),
    "VREG_AND_CHIP_RESET": (
        {0x00, 0x04},
        "VREG and BOD are stored; the regulator voltage and the brown-out detector are not simulated (CHIP_RESET acts)",
    ),
    "BUSCTRL": (
        {0x000, 0x00C, 0x014, 0x01C, 0x024, 0x008, 0x010, 0x018, 0x020},
        "BUS_PRIORITY, PERFSEL0-3 are stored and PERFCTR0-3 stay 0: there is no arbitration and no bus-fabric events to count (peripherals/_busctrl.py)",
    ),
    "UART": ({0x020}, "UARTILPR (IrDA low-power counter) is stored: there is no IrDA (peripherals/_uart.py)"),
    "TIMER": ({0x2C}, "DBGPAUSE is stored: there is no debug halt for the timer to pause on (peripherals/_timer.py)"),
    "PLL": (
        {0x04},
        "PWR (PD, VCOPD, POSTDIVPD, DSMPD) is stored: the PLL is always locked and running (peripherals/pll.py)",
    ),
    "XOSC": ({0x1C}, "COUNT is loaded by a write but never counts down (peripherals/_xosc.py)"),
    "PADS_BANK0": ({0x00}, "VOLTAGE_SELECT is stored (peripherals/pads.py)"),
}


def stub_reason(block: str) -> str:
    entry = STUBS.get(block)
    return entry[1] if entry else ""


def state(block: str, offset: int) -> tuple[str, str]:
    """(status, note) for a register that answers: Acts | Stub | Partial | Unchecked."""
    entry = STUBS.get(block)
    if entry is not None:
        offsets = entry[0]
        if offsets == ALL or offset in offsets:  # type: ignore[operator]
            return "Stub", ""
    note = PARTIAL.get(block, {}).get(offset)
    if note is not None:
        return "Partial", note
    if block in AUDITED:
        return "Acts", ""
    return "Unchecked", "the block was never audited"
