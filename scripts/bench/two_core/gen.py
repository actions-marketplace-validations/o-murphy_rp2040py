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
print("ok")
