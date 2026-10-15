"""Live check that the CPU's simulated time follows clk_sys (docs/tasks/clocks-port.md, decision 2).

    uv run python scripts/debug/live_freq.py [--image 1.29.0]

Boots MicroPython, times the same busy loop with `time.ticks_us()` (the TIMER, which ticks off clk_ref and does not move with clk_sys) at 125 MHz, then after
`machine.freq(250_000_000)`, then back at 125 MHz: the loop must take about half the simulated time at 250 MHz, and the same time again at 125 MHz.
"""

import argparse
import asyncio
import sys

from rp2040py.boards import resolve_board_spec
from rp2040py.device import MicroPythonDevice
from rp2040py.utils.firmware_retrieve import MICROPYTHON

CODE = """
import machine, time
def spin():
    t = time.ticks_us()
    for i in range(20000):
        pass
    return time.ticks_diff(time.ticks_us(), t)
a = spin()
machine.freq(250_000_000)
b = spin()
machine.freq(125_000_000)
c = spin()
print(machine.freq(), a, b, c)
"""


async def main(image: str) -> int:
    board = resolve_board_spec("pico", MICROPYTHON, image)
    device = MicroPythonDevice(board=board)
    await device.astart(timeout=180.0)
    try:
        stdout, _stderr = await device.aexec(CODE, timeout=180.0)
        freq, a, b, c = (int(token) for token in stdout.decode().split())
        print(
            f"125 MHz: {a} us, 250 MHz: {b} us (x{a / b:.2f}), back at 125 MHz: {c} us (x{c / a:.2f}) - machine.freq() says {freq}"
        )
        return 0 if 1.7 < a / b < 2.3 and 0.9 < c / a < 1.1 else 1
    finally:
        device.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--image", default="1.29.0")
    sys.exit(asyncio.run(main(parser.parse_args().image)))
