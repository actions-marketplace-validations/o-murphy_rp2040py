"""The two-core lockstep oracle (docs/records/0104-second-core-seams-and-architecture.md, P2): the pure-Python chip with its loop for two cores (`_execute_batch._run_batch_dual`) against the native chip
with the C++ one (`core/batch_dual.hpp`), on small hand-assembled two-core programs, under both schedulers - the quantum with synchronisation points (the main one, 16 cycles) and the exact one
(quantum 1). After each program everything observable must be identical: both cores' registers and cycle counts, the simulated time, the words the programs left in shared RAM, and what the
SIO FIFOs hold. The programs launch core 1 with the boot ROM's handshake (answered by the SIO), pass words over the FIFOs, divide on both cores at once (each core has its own divider) and
wake a core asleep in a WFE with a SEV."""

import pytest
from utils.chip_pair import Native, PurePython, make_chip

from rp2040py._execute_batch import execute_batch as pure_execute_batch
from rp2040py.native._simulator import execute_batch as native_execute_batch
from rp2040py.simulator import Simulator

SIO = 0xD0000000
FIFO_ST, FIFO_WR, FIFO_RD = 0x50, 0x54, 0x58
DIV_UDIVIDEND, DIV_UDIVISOR, DIV_QUOTIENT = 0x60, 0x64, 0x70
CODE0, CODE1, STACK1 = 0x20000000, 0x20000200, 0x20001000
RESULTS = 0x20000300
BKPT = 0xBE00


class Asm:
    """Just enough Thumb: what the programs below use."""

    def __init__(self) -> None:
        self.h: list[int] = []

    def here(self) -> int:
        return len(self.h)

    def emit(self, value: int) -> None:
        self.h.append(value & 0xFFFF)

    def movs(self, rd: int, imm: int) -> None:
        self.emit(0x2000 | rd << 8 | imm)

    def adds(self, rd: int, imm: int) -> None:
        self.emit(0x3000 | rd << 8 | imm)

    def ldr(self, rt: int, rn: int, byte_offset: int) -> None:
        self.emit(0x6800 | (byte_offset // 4) << 6 | rn << 3 | rt)

    def str_(self, rt: int, rn: int, byte_offset: int) -> None:
        self.emit(0x6000 | (byte_offset // 4) << 6 | rn << 3 | rt)

    def tst(self, rn: int, rm: int) -> None:
        self.emit(0x4200 | rm << 3 | rn)

    def beq(self, to: int) -> None:
        self.emit(0xD000 | ((to - (self.here() + 2)) & 0xFF))

    def b(self, to: int) -> None:
        self.emit(0xE000 | ((to - (self.here() + 2)) & 0x7FF))

    def nop(self) -> None:
        self.emit(0xBF00)

    def sev(self) -> None:
        self.emit(0xBF40)

    def wfe(self) -> None:
        self.emit(0xBF20)

    def literal(self, value: int) -> None:
        self.emit(value)
        self.emit(value >> 16)

    def wait_valid(self) -> None:
        """Wait until FIFO_ST.VLD (r1 and r2 scratch, r0 the SIO base)."""
        top = self.here()
        self.ldr(1, 0, FIFO_ST)
        self.movs(2, 1)
        self.tst(1, 2)
        self.beq(top)

    def load_literals(self, registers: list[int], literals: list[int]) -> None:
        """`ldr rN, [pc, #..]` for each register from a pool placed at the end: call `finish` after the code."""
        self._pool_loads = [(self.here() + i, reg) for i, reg in enumerate(registers)]
        self._literals = literals
        for _ in registers:
            self.emit(0)  # patched in finish()

    def finish(self) -> list[int]:
        if self.here() & 1:
            self.nop()
        pool = self.here()
        for value in self._literals:
            self.literal(value)
        for i, (at, reg) in enumerate(self._pool_loads):
            pc = (2 * at + 4) & ~3
            self.h[at] = 0x4800 | reg << 8 | ((2 * (pool + 2 * i) - pc) // 4)
        return self.h


def core1_echo_plus_one() -> list[int]:
    a = Asm()
    a.load_literals([0], [SIO])
    loop = a.here()
    a.wait_valid()
    a.ldr(3, 0, FIFO_RD)
    a.adds(3, 1)
    a.str_(3, 0, FIFO_WR)
    a.b(loop)
    return a.finish()


def _launch(a: Asm) -> None:
    """The handshake of datasheet 2.8.2 with r0 = SIO, r4/r5/r6 = vector table, stack, entry."""

    def send(reg: int) -> None:
        a.str_(reg, 0, FIFO_WR)
        a.wait_valid()
        a.ldr(1, 0, FIFO_RD)  # the echo

    a.movs(3, 0)
    send(3)
    send(3)
    a.movs(3, 1)
    send(3)
    send(4)
    send(5)
    send(6)


def core0_launch_and_ping() -> list[int]:
    a = Asm()
    a.load_literals([0, 4, 5, 6], [SIO, CODE0, STACK1, CODE1 | 1])
    _launch(a)
    a.movs(3, 5)
    a.str_(3, 0, FIFO_WR)
    a.wait_valid()
    a.ldr(7, 0, FIFO_RD)
    a.emit(BKPT)
    return a.finish()


def core0_divides_while_core1_divides() -> list[int]:
    """Both cores divide (100 / 9 and 200 / 7) and store their own quotient in shared RAM; core 0 launches core 1, divides, waits for core 1's word in RAM, then spins."""
    a = Asm()
    a.load_literals([0, 4, 5, 6, 7], [SIO, CODE0, STACK1, CODE1 | 1, RESULTS])
    _launch(a)
    a.movs(1, 100)
    a.str_(1, 0, DIV_UDIVIDEND)
    a.movs(1, 9)
    a.str_(1, 0, DIV_UDIVISOR)
    a.ldr(1, 0, DIV_QUOTIENT)
    a.str_(1, 7, 0)
    top = a.here()
    a.ldr(2, 7, 4)  # core 1's quotient, through shared RAM (no device access: the quantum may show it late)
    a.movs(3, 0)
    a.tst(2, 2)
    a.beq(top)
    a.emit(BKPT)
    return a.finish()


def core1_divides() -> list[int]:
    a = Asm()
    a.load_literals([0, 7], [SIO, RESULTS])
    a.movs(1, 200)
    a.str_(1, 0, DIV_UDIVIDEND)
    a.movs(1, 7)
    a.str_(1, 0, DIV_UDIVISOR)
    a.ldr(1, 0, DIV_QUOTIENT)
    a.str_(1, 7, 4)
    spin = a.here()
    a.b(spin)
    return a.finish()


def core0_launches_then_sevs() -> list[int]:
    a = Asm()
    a.load_literals([0, 4, 5, 6], [SIO, CODE0, STACK1, CODE1 | 1])
    _launch(a)
    for _ in range(30):
        a.nop()  # (core 1 reaches its WFE first)
    a.sev()
    for _ in range(30):
        a.nop()  # (core 1 wakes and marks r1)
    a.emit(BKPT)
    return a.finish()


def core1_wfe_then_marks() -> list[int]:
    a = Asm()
    a.wfe()
    a.movs(1, 42)
    spin = a.here()
    a.b(spin)
    return a.h


def core0_launches_a_core_that_returns() -> list[int]:
    a = Asm()
    a.load_literals([0, 4, 5, 6], [SIO, CODE0, STACK1, CODE1 | 1])
    _launch(a)
    for _ in range(60):
        a.nop()  # (core 1 marks r1, returns to the ROM and is parked there)
    a.emit(BKPT)
    return a.finish()


def core1_marks_and_returns() -> list[int]:
    """An entry function that returns: `bx lr` goes to where the launch left LR - the boot ROM."""
    a = Asm()
    a.load_literals([7], [RESULTS])
    a.movs(1, 42)
    a.str_(1, 7, 0)
    a.emit(0x4770)  # bx lr
    return a.finish()


PROGRAMS = {
    "launch and ping-pong": (core0_launch_and_ping, core1_echo_plus_one),
    "both cores divide": (core0_divides_while_core1_divides, core1_divides),
    "sev wakes a wfe": (core0_launches_then_sevs, core1_wfe_then_marks),
    "an entry function returns": (core0_launches_a_core_that_returns, core1_marks_and_returns),
}


def _run(kind, loop, program, quantum: int):
    chip = make_chip(kind)
    chip.set_core1_enabled(True)
    chip.quantum_cycles = quantum
    simulator = Simulator(rp2040=chip)
    core0_code, core1_code = PROGRAMS[program]
    for base, halfwords in ((CODE0, core0_code()), (CODE1, core1_code())):
        for i, halfword in enumerate(halfwords):
            chip.write_uint16(base + 2 * i, halfword)
    chip.core.pc = CODE0
    chip.core.registers[13] = 0x20004000
    simulator.stopped = False
    for _ in range(
        400
    ):  # a batch also ends on the real-time budget, on a launch and on a hold: resume until core 0 has stopped the simulator (BKPT)
        loop(simulator, 1)
        if simulator.stopped:
            break
    return chip


def _finished(program: str, chip) -> bool:
    if program == "launch and ping-pong":
        return chip.core.registers[7] == 6
    if program == "both cores divide":
        return chip.read_uint32(RESULTS) != 0 and chip.read_uint32(RESULTS + 4) != 0
    if program == "an entry function returns":
        return chip.read_uint32(RESULTS) == 42 and not chip.core1_running  # parked: back where a reset leaves it
    return chip.core1.registers[1] == 42


def _state(chip):
    return (
        [list(chip.cores[i].registers) for i in range(2)],
        [chip.cores[i].cycles for i in range(2)],
        [chip.cores[i].waiting for i in range(2)],
        chip.core1_running,
        chip.clock.ticks,
        chip.read_uint32(RESULTS),
        chip.read_uint32(RESULTS + 4),
        chip.read_uint32(SIO + FIFO_ST),
    )


@pytest.mark.parametrize("quantum", [16, 1])
@pytest.mark.parametrize("program", list(PROGRAMS))
def test_the_two_loops_run_two_cores_identically(program, quantum):
    pure = _run(PurePython, pure_execute_batch, program, quantum)
    native = _run(Native, native_execute_batch, program, quantum)
    assert _finished(program, pure) and _finished(program, native), (
        "the program did not finish: the test would compare nothing"
    )
    assert _state(pure) == _state(native)


def test_the_programs_do_what_they_claim():
    native = _run(Native, native_execute_batch, "both cores divide", 16)
    assert native.read_uint32(RESULTS) == 11  # 100 / 9 by core 0's divider
    assert native.read_uint32(RESULTS + 4) == 28  # 200 / 7 by core 1's own
