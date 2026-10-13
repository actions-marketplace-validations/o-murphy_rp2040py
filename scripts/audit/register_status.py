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
    "VREG_AND_CHIP_RESET", "BUSCTRL", "ROSC", "XIP", "USB", "IO_BANK0", "IO_QSPI", "PADS_BANK0", "PADS_QSPI",
}  # fmt: skip
# Blocks never audited: whatever they answer is unchecked (none left since the USB port)
UNAUDITED: set[str] = set()

# block -> {offset: what is not acted on} for registers that act but not in every field (from 0098's backlog and the second passes)
PARTIAL: dict[str, dict[int, str]] = {
    "IO_BANK0": {
        **{
            offset: "PROC1 / DORMANT_WAKE: stored; there is no second core and no dormant mode to raise a line"
            for offset in range(0x130, 0x190, 4)
        },
    },
    "IO_QSPI": {
        **{
            offset: "PROC1 / DORMANT_WAKE: stored; there is no second core and no dormant mode to raise a line"
            for offset in range(0x40, 0x58, 4)
        },
    },
    "ROSC": {
        0x00: "CTRL: DISABLE is stored, the oscillator keeps running; STABLE follows ENABLE at once",
        0x04: "FREQA: the drive strengths are validated and stored; they do not change the frequency (no figures in the datasheet)",
        0x08: "FREQB: the drive strengths are validated and stored; they do not change the frequency (no figures in the datasheet)",
        0x0C: "DORMANT: stored, nothing goes dormant",
        0x14: "PHASE: validated and stored; there is no phase-shifted output",
    },
    "M0PLUS": {0xED0C: "AIRCR: SYSRESETREQ resets the core only; VECTCLRACTIVE (a debug bit) is ignored"},
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
        0x004: "MS: slave mode is not modelled (a word has no time on the wire as a slave)",
    },
    "ADC": {0x000: "TS_EN does not gate channel 4; the SAR error flag is never raised"},
    "DMA": {
        0x00C: "CH0_CTRL_TRIG: HIGH_PRIORITY and SNIFF_EN are stored; READ_ERROR/WRITE_ERROR/AHB_ERROR are never set"
    },
    "PIO": {
        0x038: "INPUT_SYNC_BYPASS has no effect",
    },
    "I2C": {0x000: "IC_CON: the slave bits (IC_SLAVE_DISABLE, IC_10BITADDR_SLAVE) do nothing: no slave mode"},
    "PLL": {0x000: "CS.LOCK is always 1 (no start-up time)"},
    "SIO": {
        0x000: "CPUID is always 0 (no core 1)",
        0x004: "GPIO_IN reads the pad at once: the two-cycle input synchroniser is not modelled (SYSCFG.PROC_IN_SYNC_BYPASS is stored)",
        0x008: "GPIO_HI_IN reads the pad at once: the two-cycle input synchroniser is not modelled (SYSCFG.PROC_IN_SYNC_BYPASS_HI is stored)",
    },
    "PADS_BANK0": {
        0x04: "GPIOn: IE, PUE, PDE and OD act; DRIVE, SLEWFAST and SCHMITT are stored: there is no electrical model"
    },
    "PADS_QSPI": {
        offset: "GPIO_QSPI_*: IE, PUE, PDE and OD act; DRIVE, SLEWFAST and SCHMITT are stored: there is no electrical model"
        for offset in range(0x04, 0x1C, 4)
    },
    "USB": {
        0x000: "ADDR_ENDP: acts as the host's device address; in device mode the value is only stored",
    },
    "XIP": {
        0x000: "CTRL.ERR_BADWRITE is never set: a write to a non-caching alias warns instead of raising the bus fault (record 0098, XIP_CTRL pass)"
    },
    "XOSC": {
        0x008: "DORMANT is stored; the oscillator does not go dormant",
        0x00C: "STARTUP delay is stored; STABLE is set at once, not after the delay",
    },
    "WATCHDOG": {0x000: "CTRL: PAUSE_DBG0, PAUSE_DBG1, PAUSE_JTAG are stored; there is no debug halt to pause on"},
}
# block -> (offsets that are stubs | ALL, why)
STUBS: dict[str, tuple[object, str]] = {
    "SSI": (
        {ssi.SSI_MWCR, ssi.SSI_SER, 0x00, 0x04, 0xF0, 0xF4, 0xF8},
        "stored only (peripherals/_ssi.py): MWCR, SER, CTRLR1, RX_SAMPLE_DLY, SPI_CTRLR0 and TXD_DRIVE_EDGE; CTRLR0 acts only through TMOD = transmit only (the frame size, SPI format and the other modes are not modelled); the FIFOs, thresholds, interrupts, DMACR and the DMA levels act since the XIP_CTRL port (BAUDR paces the XIP stream)",
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
        "only PROC0_NMI_MASK has an effect (peripherals/_syscfg.py); the rest is stored: no second core, no debug port, no memory to power down, and PROC_IN_SYNC_BYPASS(_HI) bypass a synchroniser whose two cycles of latency are not modelled",
    ),
    "VREG_AND_CHIP_RESET": (
        {0x00, 0x04},
        "VREG and BOD are stored; the regulator voltage and the brown-out detector are not simulated (CHIP_RESET acts)",
    ),
    "BUSCTRL": (
        {0x000, 0x00C, 0x014, 0x01C, 0x024, 0x008, 0x010, 0x018, 0x020},
        "BUS_PRIORITY, PERFSEL0-3 are stored and PERFCTR0-3 stay 0: there is no arbitration and no bus-fabric events to count (peripherals/_busctrl.py)",
    ),
    "M0PLUS": (
        {0xED10, 0xED94, 0xED98, 0xED9C, 0xEDA0},
        "SCR (SLEEPONEXIT, SLEEPDEEP, SEVONPEND) and the MPU (CTRL, RNR, RBAR, RASR) are stored and read back as the datasheet lays them out; nothing acts on them: WFI ignores SLEEPONEXIT and SEVONPEND, and no access is ever checked against a region (peripherals/_ppb.py)",
    ),
    "UART": ({0x020}, "UARTILPR (IrDA low-power counter) is stored: there is no IrDA (peripherals/_uart.py)"),
    "TIMER": ({0x2C}, "DBGPAUSE is stored: there is no debug halt for the timer to pause on (peripherals/_timer.py)"),
    "PLL": (
        {0x04},
        "PWR (PD, VCOPD, POSTDIVPD, DSMPD) is stored: the PLL is always locked and running (peripherals/pll.py)",
    ),
    "XOSC": ({0x1C}, "COUNT is loaded by a write but never counts down (peripherals/_xosc.py)"),
    "PADS_BANK0": (
        {0x00, 0x7C, 0x80},
        "VOLTAGE_SELECT, SWCLK and SWD are stored (core/pads.hpp): the voltage thresholds and the debug pads have no electrical model",
    ),
    "PADS_QSPI": ({0x00}, "VOLTAGE_SELECT is stored (core/pads.hpp)"),
    "USB": (
        {0x6C, 0x70},
        "NAK_POLL and EP_STATUS_STALL_NAK are stored and nothing acts on them: the shortcut wire has no NAK or STALL handshake (record 0098, USB)",
    ),
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
