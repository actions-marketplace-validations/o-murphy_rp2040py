"""The Thumb programs of proto.cpp (docs/records/0104, P0.5): two ALU loops, a ping-pong through shared RAM, and a WFI.

Run it from a scratch directory (it writes alu0.bin alu1.bin pp0.bin pp1.bin wfi.bin there), then build and run proto.cpp from the same directory:

    python <repo>/scripts/bench/two_core/gen.py
    c++ -std=c++17 -O2 -fno-exceptions -fno-rtti -ffp-contract=off -I<repo>/src/rp2040py/native/core <repo>/scripts/bench/two_core/proto.cpp -o proto && ./proto
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import synthetic as s  # (scripts/bench/synthetic.py)


def alu(offset, shift=22):
    setup = [s._movs(7, 0x20), s._lsls(7, 7, 24), s._movs(5, 1), s._lsls(5, 5, shift), s._movs(0, 0), s._movs(2, 0)]
    loop = [
        s._adds_imm3(0, 0, 1),
        s._eors(2, 0),
        s._lsls(3, 0, 2),
        s._str(3, 7, offset),
        s._ldr(4, 7, offset),
        s._adds_reg(2, 2, 4),
    ]
    loop += [s._subs_imm8(5, 1)]
    loop.append(s._bne(-(len(loop) * 2 + 4)))
    return setup + loop + [s.BKPT]


def put(name, halfwords):
    Path(name).write_bytes(b"".join(struct.pack("<H", h) for h in halfwords))


put("alu0.bin", alu(64))
put("alu1.bin", alu(96))
setup = [0x2020, 0x0600, 0x2202, 0x0412, 0x1880, 0x2100]  # r0 = 0x20020000, r1 = 0
put("pp0.bin", setup + [0x1C49, 0x6001, 0x6842, 0x428A, 0xD1FC, 0xE7F9])
put("pp1.bin", setup + [0x6802, 0x428A, 0xD0FC, 0x1C11, 0x6041, 0xE7F9])
put("wfi.bin", [0xBF30, 0xE7FE])

# Core 0 reads the TIMER's microsecond counter in a loop (r0 = 0x40054000, TIMELR at +0x0C): the accuracy probe of the clock inside a scheduling quantum.
put("tm0.bin", [0x2040, 0x0600, 0x2254, 0x0312, 0x1880, 0x2100, 0x68C3, 0x1C49, 0xE7FC])

# Ping-pong through the SIO spinlocks, the way SDK firmware synchronises: r4 = SIO + 0x100 (SPINLOCK0 at [r4], SPINLOCK1 at [r4, #4]), r5 = the shared counter's address.
# The harness starts with both locks held. Core 0 releases lock 1 and spins to claim lock 0 (and counts a round trip); core 1 spins to claim lock 1 and releases lock 0.
sio_setup = [0x20D0, 0x0600, 0x2401, 0x0224, 0x1824, 0x2520, 0x062D, 0x2602, 0x0436, 0x19AD, 0x2100, 0x2200]
put("sl0.bin", sio_setup + [0x6062, 0x6823, 0x2B00, 0xD0FC, 0x1C49, 0x6029, 0xE7F8])
put("sl1.bin", sio_setup + [0x6863, 0x2B00, 0xD0FC, 0x6022, 0xE7FA])
print("ok")
