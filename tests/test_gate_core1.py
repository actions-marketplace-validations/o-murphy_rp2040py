"""The second core on the bare C++ core (docs/records/0104-second-core-seams-and-architecture.md, P1): a stock MicroPython starts a thread - `_thread.start_new_thread` on the RP2040 is
`multicore_launch_core1`, the handshake of datasheet 2.8.2 over the inter-core FIFOs - and the thread runs on core 1. No Python and no Cython is in the harness (tests/cpp/gate_boot.cpp), so this
is the C++ core's second core alone, under both schedulers: the quantum with synchronisation points (the main one) and the exact one (quantum 1)."""

import pytest
from utils import gate_boot

pytestmark = [
    pytest.mark.skipif(gate_boot.cxx() is None and not gate_boot.required(), reason="no C++ compiler on PATH"),
]

MAIN_PY = b"""\
import machine, _thread

uart = machine.UART(0, 115200, tx=machine.Pin(0), rx=machine.Pin(1))
result = []


def work():
    result.append(sum(i * i for i in range(100)))


uart.write(b"A")
_thread.start_new_thread(work, ())
uart.write(b"B")
spins = 0
while not result and spins < 2000000:
    spins += 1
uart.write(b"C")
uart.write(("THREAD %d\\r\\n" % result[0]).encode())
"""
EXPECTED = "THREAD 328350"  # sum of the squares of 0..99


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    directory = tmp_path_factory.mktemp("core1")
    rom, flash = gate_boot.micropython_images(directory, MAIN_PY)
    return gate_boot.build_harness(directory), rom, flash


@pytest.mark.parametrize("quantum", ["16", "1"])
def test_micropython_runs_a_thread_on_core_1(harness, quantum):
    exe, rom, flash = harness
    done = gate_boot.run(exe, rom, flash, EXPECTED, env={"RP2040PY_CORE1": "1", "RP2040PY_QUANTUM": quantum})
    assert done.stdout.startswith("ABC"), done.stdout  # the thread was started and the main thread went on
    # the chip asked the world for nothing but the bytes it printed: no warning (the FIFO registers are implemented now)
    callbacks = gate_boot.callbacks(done)
    assert len(callbacks) == 1 and callbacks[0].startswith("callback uart_byte="), done.stderr


def test_without_the_second_core_thread_start_does_not_return(harness):
    exe, rom, flash = harness
    # the default: the FIFO registers are the old stub, the handshake never completes - what the pure-Python reference still does
    done = gate_boot.subprocess.run(
        [str(exe), str(rom), str(flash), "20", EXPECTED], capture_output=True, text=True, check=False, timeout=600
    )
    assert done.stdout == "A"
    assert "callback warn block=" in done.stderr
