"""The CPU's simulated time per instruction is `1 / clk_sys` (docs/tasks/clocks-port.md, decision 2): an overclock makes the same code take less *simulated* time, an underclock more - the emulator
does not run faster or slower in wall time, it simulates what the firmware's clock tree asks for.

The firmware here is a few instructions in SRAM that write `CLK_SYS_DIV` in the middle of a run of NOPs. Both batch loops (the C++ one and the pure-Python reference) must charge the instructions before
the write at the old period and the ones after it at the new one - the very next instruction, not the next batch.
"""

import pytest
from utils.chip_pair import Native, make_chip

pytest.importorskip("rp2040py.native._simulator", reason="the native extension is not built")

from rp2040py._native_gate import native_disabled

pytestmark = pytest.mark.skipif(
    native_disabled(), reason="RP2040PY_SKIP_CYTHON=1: the facade gives the chip pure-Python PIO blocks"
)

from rp2040py._execute_batch import execute_batch as pure_execute_batch
from rp2040py.native._simulator import execute_batch as native_execute_batch
from rp2040py.simulator import Simulator
from rp2040py.utils.assembler import opcode_nop, opcode_str

CODE = 0x20000000
BKPT = 0xBE00
CLOCKS = 0x40008000
CLK_SYS_DIV = CLOCKS + 0x40
NOPS_BEFORE, NOPS_AFTER = 40, 200
LOOPS = [pytest.param(native_execute_batch, id="native"), pytest.param(pure_execute_batch, id="pure")]


def _configure_125mhz(chip) -> None:
    """The SDK's clocks_init() for 12 MHz * 125 / (6 * 2) = 125 MHz on PLL_SYS."""
    chip.write_uint32(0x40024000, (0xFAB << 12) | 0xAA0)  # xosc_init()
    chip.write_uint32(0x40028000, 1)  # PLL_SYS: REFDIV = 1
    chip.write_uint32(0x40028008, 125)
    chip.write_uint32(0x40028004, 0)
    chip.write_uint32(0x4002800C, (6 << 16) | (2 << 12))
    chip.write_uint32(CLOCKS + 0x30, 2)  # clk_ref from the crystal
    chip.write_uint32(CLOCKS + 0x3C, 1)  # clk_sys from its aux source, PLL_SYS
    assert chip.clk_sys == 125e6


def _run(loop, divider: int, tick_batch: int):
    chip = make_chip(Native)
    _configure_125mhz(chip)
    program = [opcode_nop()] * NOPS_BEFORE + [opcode_str(1, 0, 0)] + [opcode_nop()] * NOPS_AFTER + [BKPT]
    for i, opcode in enumerate(program):
        chip.write_uint16(CODE + 2 * i, opcode)
    chip.core.pc = CODE
    chip.core.registers[0] = CLK_SYS_DIV
    chip.core.registers[1] = divider
    chip.core.registers[13] = 0x20001000
    simulator = Simulator(rp2040=chip)
    start = chip.clock.nanos
    simulator.stopped = False
    for _ in range(200):
        loop(simulator, tick_batch)
        if simulator.stopped:
            break
    assert simulator.stopped
    return chip, chip.clock.nanos - start


def _cycles_up_to_and_including_the_write() -> int:
    chip = make_chip(Native)
    _configure_125mhz(chip)
    for i, opcode in enumerate([opcode_nop()] * NOPS_BEFORE + [opcode_str(1, 0, 0)]):
        chip.write_uint16(CODE + 2 * i, opcode)
    chip.core.pc = CODE
    chip.core.registers[0] = (
        CLK_SYS_DIV  # the same store (a peripheral access costs more than one to SRAM), of the value it already has
    )
    chip.core.registers[1] = 0x100
    cycles = 0
    for _ in range(NOPS_BEFORE + 1):
        cycles += chip.core.execute_instruction()
    return cycles


@pytest.mark.parametrize("tick_batch", [1, 8])
@pytest.mark.parametrize("loop", LOOPS)
def test_an_underclock_mid_run_charges_the_instructions_after_it_at_the_new_period(loop, tick_batch):
    kept, kept_nanos = _run(loop, 0x100, tick_batch)  # the same value again: no change of clk_sys
    halved, halved_nanos = _run(loop, 0x200, tick_batch)  # clk_sys / 2 from the write on
    assert kept.clk_sys == 125e6 and halved.clk_sys == 62.5e6
    assert kept.core.cycles == halved.core.cycles  # the same instructions ran
    after = kept.core.cycles - _cycles_up_to_and_including_the_write()
    assert after > NOPS_AFTER  # (the BKPT is one more)
    # 8 ns per cycle at 125 MHz, 16 ns at 62.5 MHz: the cycles after the write take 8 ns longer each
    assert halved_nanos - kept_nanos == pytest.approx(8.0 * after, abs=1e-6)


@pytest.mark.parametrize("loop", LOOPS)
def test_a_clock_that_never_changes_keeps_the_125_mhz_period(loop):
    chip, nanos = _run(loop, 0x100, 1)
    assert nanos == pytest.approx(8.0 * chip.core.cycles, abs=1e-6)


@pytest.mark.parametrize("loop", LOOPS)
def test_an_overclock_makes_the_same_code_take_less_simulated_time(loop):
    chip = make_chip(Native)
    _configure_125mhz(chip)
    chip.write_uint32(CLOCKS + 0x40, 0x100)
    chip.write_uint32(0x40028008, 250)  # PLL_SYS at 250 MHz
    assert chip.clk_sys == 250e6
    program = [opcode_nop()] * 100 + [BKPT]
    for i, opcode in enumerate(program):
        chip.write_uint16(CODE + 2 * i, opcode)
    chip.core.pc = CODE
    chip.core.registers[13] = 0x20001000
    simulator = Simulator(rp2040=chip)
    start = chip.clock.nanos
    simulator.stopped = False
    for _ in range(200):
        loop(simulator, 1)
        if simulator.stopped:
            break
    assert chip.clock.nanos - start == pytest.approx(4.0 * chip.core.cycles, abs=1e-6)  # 4 ns per cycle at 250 MHz
