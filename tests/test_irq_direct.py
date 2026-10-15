"""The interrupt lines of the native blocks go straight to the CPU, and a replaced `set_interrupt` still sees them (docs/records/0096-cpp-mcu-core.md, step 2b).

Written once for both builds: on the pure-Python chip a line is a call of `RP2040.set_interrupt`; on the native one it is a C++ call of `Mcu::set_interrupt` with no Python between the block and the
CPU, *unless* `set_interrupt` is assigned - then the assigned function gets the line instead of the CPU, as it always did. Same observable behaviour either way, which is what is checked.
"""

import pytest

from rp2040py.irq import IRQ
from rp2040py.rp2040 import RP2040

UARTDR, UARTCR, UARTIMSC = 0x00, 0x30, 0x38
UART_ENABLED, UARTTXINTR = 0x301, 1 << 5


def _raise_the_tx_interrupt(chip: RP2040) -> None:
    uart = chip.uart[0]
    uart.write_uint32(UARTCR, UART_ENABLED)
    uart.write_uint32(UARTIMSC, UARTTXINTR)
    uart.write_uint32(UARTDR, 0x41)  # a written byte raises the TX interrupt


def _pending(chip: RP2040, irq: IRQ) -> bool:
    return bool(chip.core.pending_interrupts & (1 << irq))


def test_a_block_raises_its_line_on_the_cpu():
    chip = RP2040()
    assert not _pending(chip, IRQ.UART0)
    _raise_the_tx_interrupt(chip)
    assert _pending(chip, IRQ.UART0)
    assert not _pending(chip, IRQ.UART1)


def test_a_replaced_set_interrupt_gets_the_line_instead_of_the_cpu():
    chip = RP2040()
    seen = []
    chip.set_interrupt = lambda irq, level: seen.append((irq, bool(level)))
    _raise_the_tx_interrupt(chip)
    assert (IRQ.UART0, True) in seen
    assert not _pending(chip, IRQ.UART0), "the spy did not pass it on, so the CPU never saw it"


def test_a_spy_that_passes_the_line_on_leaves_the_cpu_as_it_was():
    chip = RP2040()
    original, seen = chip.set_interrupt, []

    def spy(irq, level):
        seen.append((irq, bool(level)))
        original(irq, level)

    chip.set_interrupt = spy
    _raise_the_tx_interrupt(chip)
    assert (IRQ.UART0, True) in seen
    assert _pending(chip, IRQ.UART0)


def test_taking_the_replacement_away_gives_the_lines_back_to_the_cpu():
    chip = RP2040()
    chip.set_interrupt = lambda irq, level: None
    del chip.set_interrupt
    _raise_the_tx_interrupt(chip)
    assert _pending(chip, IRQ.UART0)


def test_a_failing_replacement_surfaces_from_the_access_that_raised_the_line():
    chip = RP2040()

    class Boom(Exception):
        pass

    def fail(irq, level):
        raise Boom((irq, level))

    chip.set_interrupt = fail
    with pytest.raises(Boom):
        _raise_the_tx_interrupt(chip)


def test_the_chip_still_calls_the_replacement_for_the_io_bank_line():
    chip = RP2040()
    seen = []
    chip.set_interrupt = lambda irq, level: seen.append((irq, bool(level)))
    chip.update_io_interrupt()
    assert seen == [(IRQ.IO_BANK0, False)]


def test_a_pin_raises_the_io_bank_line_on_the_cpu():
    chip = RP2040()
    chip.gpio[5].set_input_value(True)
    assert not _pending(chip, IRQ.IO_BANK0)
    chip.write_uint32(0x40014000 + 0x100, 0x2 << (5 * 4))  # PROC0_INTE0: pin 5, level high
    assert _pending(chip, IRQ.IO_BANK0)


def test_the_io_bank_line_does_not_go_through_update_io_interrupt_on_a_native_chip():
    chip = RP2040()
    if not hasattr(chip, "_native_mcu_address"):
        pytest.skip("the pure-Python chip asks update_io_interrupt() - that is its line")

    def fail() -> None:
        raise AssertionError("the pins went through update_io_interrupt()")

    chip.update_io_interrupt = fail  # type: ignore[method-assign]
    chip.gpio[5].set_input_value(True)
    chip.write_uint32(0x40014000 + 0x100, 0x2 << (5 * 4))
    assert _pending(chip, IRQ.IO_BANK0)
