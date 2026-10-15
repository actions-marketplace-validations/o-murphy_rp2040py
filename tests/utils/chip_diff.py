"""The chip-level oracle of the reset cascade (docs/records/0096-cpp-mcu-core.md, step 3b): the pure-Python chip (`RP2040.reset()` / `enter_reset()` / `leave_reset()`, the reference) and the
native chip (the C++ `Mcu::reset()` cascade) side by side, driven in lockstep through every trigger after a random guest-side dirtying of a register of every block the cascade covers, and compared
after each step on: those registers read through the bus, the core's registers, the cause registers (the watchdog's scratch and `CHIP_RESET`), the flash, the interrupt lines and the number of times
the USB link was told of a reset.

`Rig(chip)` wraps one chip; `run(ops)` applies the same operation list to both and returns the first divergence (or None). `OPS` is the generator the tests use, `MUTANTS` are changes to the *pure*
chip's cascade (a block dropped, a gate swapped, the cause not recorded, ...) that this oracle must see.
"""

import random
from collections.abc import Callable
from typing import Any

from rp2040py.reset_cause import ResetCause

# (address, name) of one plain read/write register per block of the cascade; none of them has a read side effect. The comment says which gate covers it.
REGISTERS: tuple[tuple[int, str], ...] = (
    (0x40034024, "uart0.ibrd"),  # RESETS.UART0
    (0x40038024, "uart1.ibrd"),  # RESETS.UART1
    (0x4003C000, "spi0.cr0"),  # RESETS.SPI0
    (0x40040000, "spi1.cr0"),  # RESETS.SPI1
    (0x40044004, "i2c0.tar"),  # RESETS.I2C0
    (0x40048004, "i2c1.tar"),  # RESETS.I2C1
    (0x40050010, "pwm.ch0.top"),  # RESETS.PWM
    (0x4004C010, "adc.div"),  # RESETS.ADC
    (0x40054010, "timer.alarm0"),  # RESETS.TIMER
    (0x4005C000, "rtc.clkdiv"),  # RESETS.RTC
    (0x40030000, "busctrl.priority"),  # RESETS.BUSCTRL
    (0x50000000, "dma.ch0.read"),  # RESETS.DMA
    (0x502000C8, "pio0.sm0.clkdiv"),  # RESETS.PIO0
    (0x503000C8, "pio1.sm0.clkdiv"),  # RESETS.PIO1
    (0x50110000, "usb.addr_endp"),  # RESETS.USBCTRL
    (0x40014004 + 8 * 5, "io_bank0.gpio5.ctrl"),  # RESETS.IO_BANK0
    (0x4001C004 + 4 * 5, "pads_bank0.gpio5"),  # RESETS.PADS_BANK0
    (0x40018004 + 8 * 1, "io_qspi.ss.ctrl"),  # RESETS.IO_QSPI
    (0x40020004 + 4 * 1, "pads_qspi.ss"),  # RESETS.PADS_QSPI
    (0xD0000010, "sio.gpio_out"),  # PSM.SIO
    (0x40008004, "clocks.gpout0_div"),  # PSM.CLOCKS
    (0x4002400C, "xosc.startup"),  # PSM.XOSC
    (0x14000000, "xip.ctrl"),  # PSM.XIP
    (0x18000004, "ssi.ctrlr1"),  # PSM.XIP
    (0x4005800C, "watchdog.scratch0"),  # survives a watchdog reset, cleared by the others
)
WATCHDOG_SCRATCH0 = 0x4005800C
CHIP_RESET = 0x40064008
PSM_WDSEL = 0x40010008
RESETS_WDSEL = 0x4000C004
# the registers the cascade is told the gates in
READ_ONLY_COMPARED = (PSM_WDSEL, RESETS_WDSEL, CHIP_RESET)

CAUSES = (ResetCause.POWER_ON, ResetCause.RUN_PIN, ResetCause.WATCHDOG)


class Rig:
    """One chip and the log of what the reset cascade told the outside world."""

    def __init__(self, chip: Any) -> None:
        self.chip = chip
        self.bus_resets = 0
        self.lines: list[tuple[int, int]] = []
        chip.usb_ctrl.on_reset = self._on_bus_reset
        original = chip.set_interrupt

        def set_interrupt(irq: int, level: bool) -> None:
            self.lines.append((int(irq), int(bool(level))))
            original(irq, level)

        chip.set_interrupt = set_interrupt
        chip.flash[0:4] = b"\x12\x34\x56\x78"  # a firmware image that a live reset must keep

    def _on_bus_reset(self) -> None:
        self.bus_resets += 1

    def apply(self, op: tuple) -> None:
        kind, chip = op[0], self.chip
        if kind == "write":
            chip.write_uint32(op[1], op[2] & 0xFFFFFFFF)
        elif kind == "dirty_all":  # every register of the table, with values from the seed
            rng = random.Random(op[1])
            for address, _name in REGISTERS:
                chip.write_uint32(address, rng.getrandbits(8) | 1)
        elif kind == "wdsel":
            chip.write_uint32(PSM_WDSEL, op[1])
            chip.write_uint32(RESETS_WDSEL, op[2])
        elif kind == "reset":
            chip.reset(preserve_flash=op[1], from_watchdog=op[2])
        elif kind == "enter":
            chip.enter_reset(cause=op[1])
        elif kind == "leave":
            chip.leave_reset()
        elif kind == "run_pin":
            chip.set_run_pin(low=op[1])
        else:  # pragma: no cover
            raise AssertionError(op)

    def snapshot(self) -> dict[str, Any]:
        chip = self.chip
        state: dict[str, Any] = {name: int(chip.read_uint32(address)) for address, name in REGISTERS}
        for address in READ_ONLY_COMPARED:
            state[f"reg_{address:08x}"] = int(chip.read_uint32(address))
        state["core"] = (
            [int(chip.core.registers[i]) for i in range(16)] if hasattr(chip.core, "registers") else int(chip.core.pc)
        )
        state["flash"] = bytes(chip.flash[0:4])
        state["bus_resets"] = self.bus_resets
        state["lines"] = list(self.lines)
        return state


def diff(reference: dict[str, Any], native: dict[str, Any]) -> list[str]:
    return [
        f"{key}: pure {reference[key]!r} != native {native[key]!r}"
        for key in reference
        if reference[key] != native[key]
    ]


def make_ops(seed: int, length: int = 24) -> list[tuple]:
    """A random sequence: dirty every block, pick a trigger, sometimes with WDSEL registers drawn at random (single bits and the RESETS bit are the interesting ones)."""
    rng = random.Random(seed)
    ops: list[tuple] = []
    for _ in range(length):
        ops.append(("dirty_all", rng.getrandbits(32)))
        if rng.random() < 0.6:
            psm = rng.choice((0, 0x8, 0x1000, 0x4000, 0x4, 0x2, 0x1, 0x1FFFC, rng.getrandbits(17)))
            resets = rng.choice((0, rng.getrandbits(25), 1 << rng.randrange(25), 0x01FFFFFF))
            ops.append(("wdsel", psm, resets))
        trigger = rng.choice(("watchdog", "run", "power", "reset_plain", "reset_wd", "pin"))
        if trigger == "watchdog":
            ops.append(("enter", ResetCause.WATCHDOG))
        elif trigger == "run":
            ops.append(("enter", ResetCause.RUN_PIN))
        elif trigger == "power":
            ops.append(("enter", ResetCause.POWER_ON))
        elif trigger == "reset_plain":
            ops.append(("reset", rng.random() < 0.5, False))
        elif trigger == "reset_wd":
            ops.append(("reset", True, True))
        else:
            ops.append(("run_pin", True))
            ops.append(("dirty_all", rng.getrandbits(32)))
            ops.append(("run_pin", False))
        if rng.random() < 0.3:
            ops.append(("leave",))
    return ops


def run(pure: Any, native: Any, ops: list[tuple]) -> str | None:
    """Both chips through `ops` in lockstep; the first divergence as text, or None."""
    a, b = Rig(pure), Rig(native)
    start = diff(a.snapshot(), b.snapshot())
    if start:
        return f"before any step: {start[:3]}"
    for index, op in enumerate(ops):
        a.apply(op)
        b.apply(op)
        mismatch = diff(a.snapshot(), b.snapshot())
        if mismatch:
            return f"after step {index} {op[0]}{tuple(str(x) for x in op[1:])}: {mismatch[:4]}"
    return None


# -- mutants of the pure chip's cascade: each one must be seen ---------------------------------------------------------------------------------------


def _drop(block: str) -> Callable[[Any], None]:
    def mutate(chip: Any) -> None:
        target = (
            getattr(chip, block) if "." not in block else getattr(chip, block.split(".")[0])[int(block.split(".")[1])]
        )
        target.reset = lambda *a, **k: None

    return mutate


def _qspi_pins_kept(chip: Any) -> None:
    for pin in chip.qspi:
        pin.reset = lambda *a, **k: None


def _no_cause(chip: Any) -> None:
    chip._record_reset_cause = lambda cause: None


def _always_record(chip: Any) -> None:
    original = chip._record_reset_cause
    chip._record_reset_cause = lambda cause: original(ResetCause.RUN_PIN)


def _psm_ignored(chip: Any) -> None:
    original = chip.reset

    def reset(*, preserve_flash: bool = False, from_watchdog: bool = False) -> None:
        original(preserve_flash=preserve_flash, from_watchdog=False)

    chip.reset = reset


def _pc_wrong(chip: Any) -> None:
    chip.leave_reset = lambda: setattr(chip.core, "pc", 0x10000004)


MUTANTS: dict[str, Callable[[Any], None]] = {
    "uart0 left out": _drop("uart.0"),
    "pwm left out": _drop("pwm"),
    "dma left out": _drop("dma"),
    "qspi pins not reset": _qspi_pins_kept,
    "xip left out": _drop("xip_ctrl"),
    "cause not recorded": _no_cause,
    "every cause is a RUN-pin one": _always_record,
    "WDSEL ignored (a watchdog reset covers everything)": _psm_ignored,
    "leave_reset to the wrong address": _pc_wrong,
}
