"""The RTC built by the chip's C++ `Mcu` (docs/records/0096-cpp-mcu-core.md, step 3a, block 1): on the native chip the `Mcu` initialises the block, maps its window and binds its line, and the Python RTC is a
view of it; its warnings go through the one host interface of the `Mcu`. The pure-Python chip is the reference, so the same checks run on both.
"""

import pytest
from utils.chip_pair import Native, PurePython, make_chip

from rp2040py.irq import IRQ

RTC = 0x4005C000
CLKDIV_M1, SETUP_0, INTE, INTF = 0x00, 0x04, 0x24, 0x28


class Recorder:
    def __init__(self):
        self.messages = []

    def __getattr__(self, name):
        if name in ("debug", "info", "warning", "error"):
            return lambda component, message: self.messages.append((name, component, message))
        raise AttributeError(name)


@pytest.fixture(params=[PurePython, Native], ids=["pure", "native"])
def chip(request):
    chip = make_chip(request.param)
    chip.logger = Recorder()
    return chip


def test_a_register_that_does_not_exist_warns_through_the_logger(chip):
    chip.read_uint32(RTC + 0x3C)
    assert chip.logger.messages == [("warning", "RTC_BASE", "Unimplemented peripheral read from 0x3c")]


def test_a_write_to_a_register_that_does_not_exist_warns_with_its_value(chip):
    chip.write_uint32(RTC + 0x3C, 0x1234)
    assert chip.logger.messages == [("warning", "RTC_BASE", "Unimplemented peripheral write to 0x3c: 0x1234")]


def test_the_force_register_raises_the_line_on_the_cpu(chip):
    assert not chip.core.pending_interrupts & (1 << IRQ.RTC)
    chip.write_uint32(RTC + INTF, 1)
    assert chip.core.pending_interrupts & (1 << IRQ.RTC)
    chip.write_uint32(RTC + INTF, 0)
    assert not chip.core.pending_interrupts & (1 << IRQ.RTC)


def test_the_python_rtc_is_a_view_of_the_block_the_bus_reaches(chip):
    chip.rtc.clkdiv_m1 = 46874
    assert chip.read_uint32(RTC + CLKDIV_M1) == 46874
    chip.write_uint32(RTC + SETUP_0, (2024 << 12) | (2 << 8) | 29)
    assert chip.rtc.setup0 == (2024 << 12) | (2 << 8) | 29


def test_a_replaced_rtc_takes_the_window_over(chip):
    class Stand:
        def read_uint32(self, offset):
            return 0xABCD

        def write_uint32(self, offset, value):
            pass

        def write_uint32_atomic(self, offset, value, atomic_type):
            pass

    chip.peripherals[0x4005C] = Stand()
    assert chip.read_uint32(RTC + CLKDIV_M1) == 0xABCD


TBMAN = 0x4006C000


def test_tbman_built_by_the_chip_says_asic_and_warns_for_a_register_that_does_not_exist(chip):
    assert chip.read_uint32(TBMAN) == 1
    chip.write_uint32(TBMAN, 0xFFFFFFFF)  # read-only: no effect, no warning
    assert chip.logger.messages == []
    chip.read_uint32(TBMAN + 0x10)
    assert chip.logger.messages == [("warning", "TBMAN_BASE", "Unimplemented peripheral read from 0x10")]


@pytest.mark.parametrize(
    "base, name",
    [
        (0x40030000, "BUSCTRL_BASE"),
        (0x40064000, "VREG_AND_CHIP_RESET_BASE"),
        (0x40000000, "SYSINFO_BASE"),
        (0x40004000, "SYSCFG"),
    ],
)
def test_the_register_blocks_the_chip_built_warn_under_their_own_names(chip, base, name):
    chip.read_uint32(base + 0x3C)  # no such register in any of them
    assert chip.logger.messages == [("warning", name, "Unimplemented peripheral read from 0x3c")]


def test_sysinfo_reads_the_revision_off_the_bootrom_the_chip_holds(chip):
    chip.bootrom[4] = (chip.bootrom[4] & 0x00FFFFFF) | (3 << 24)  # ROM address 0x13: rp2040_rom_version() = 3 (B2)
    assert chip.read_uint32(0x40000000) >> 28 == 2
    chip.bootrom[4] = (chip.bootrom[4] & 0x00FFFFFF) | (1 << 24)
    assert chip.read_uint32(0x40000000) >> 28 == 1


def test_syscfg_nmi_mask_is_the_cpus_own(chip):
    chip.write_uint32(0x40004000, 0x6)
    assert chip.core.interrupt_nmi_mask == 0x6
