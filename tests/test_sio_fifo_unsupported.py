"""Core 1 is not supported, so any access to the SIO inter-core FIFO (0x50-0x58) is `multicore_launch_core1()` / `_thread` reaching for it (docs/records/0053-core1-and-inter-core-fifo.md).
Before this, the access logged a warning and read `0xFFFFFFFF`, and the SDK's launch handshake - which has no way out - went round forever. Now the C++ block stops the program with a message
(`fatal()`: no unwinding, no signal to the host, because a firmware that went on would only hang), and the pure-Python reference raises `NotImplementedError` with the same text.

The native case ends the process, so it runs in a child.
"""

import os
import subprocess
import sys

import pytest
from utils.emscripten import needs_processes

pytestmark = needs_processes  # the native case aborts the interpreter: it runs in a child process

SIO = 0xD0000000
FIFO_ST, FIFO_WR, FIFO_RD = 0x50, 0x54, 0x58
SCRIPT = (
    "import sys\n"
    "from rp2040py.rp2040 import RP2040\n"
    "chip = RP2040()\n"
    "kind, offset = sys.argv[1], int(sys.argv[2])\n"
    "chip.write_uint32(0xD0000000 + offset, 1) if kind == 'write' else chip.read_uint32(0xD0000000 + offset)\n"
    "print('survived')\n"
)


def _run(skip_cython: bool, kind: str, offset: int) -> "subprocess.CompletedProcess[str]":
    env = {**os.environ, "RP2040PY_SKIP_CYTHON": "1" if skip_cython else "0"}
    return subprocess.run(
        [sys.executable, "-c", SCRIPT, kind, str(offset)],
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
        check=False,
    )


ACCESSES = [
    ("read", FIFO_ST),
    ("read", FIFO_WR),
    ("read", FIFO_RD),
    ("write", FIFO_ST),
    ("write", FIFO_WR),
    ("write", FIFO_RD),
]


@pytest.mark.parametrize(("kind", "offset"), ACCESSES, ids=[f"{k}-{o:#x}" for k, o in ACCESSES])
def test_the_pure_python_reference_raises_with_the_message(kind, offset):
    result = _run(True, kind, offset)
    assert result.returncode != 0 and "survived" not in result.stdout
    assert "NotImplementedError" in result.stderr and "inter-core FIFO" in result.stderr and "0053" in result.stderr


@pytest.mark.parametrize(("kind", "offset"), ACCESSES, ids=[f"{k}-{o:#x}" for k, o in ACCESSES])
def test_the_native_block_stops_the_program_with_the_message(kind, offset):
    pytest.importorskip("rp2040py.native._rp2040", reason="the native extension is not built")
    result = _run(False, kind, offset)
    assert result.returncode != 0 and "survived" not in result.stdout  # aborted (SIGABRT on POSIX, exit 3 on Windows)
    assert "inter-core FIFO" in result.stderr and "core 1 is not supported" in result.stderr and "0053" in result.stderr
    assert "Traceback" not in result.stderr  # no unwinding: the process ended where it stood


def test_the_other_sio_registers_are_untouched():
    result = _run(False, "read", 0x04)  # GPIO_IN
    assert result.returncode == 0 and "survived" in result.stdout
