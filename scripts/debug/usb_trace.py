#!/usr/bin/env python3
"""Trace what a firmware does to the USB controller (every register access, DPRAM buffer-control write and finished buffer, with the simulated time) and dump the first and last accesses when
it hangs. Used to find why pico-sdk 1.2.0-1.4.0 (old TinyUSB) stopped enumerating after the USB reference followed the datasheet (record 0098, USB).

    python -I scripts/debug/usb_trace.py <firmware.uf2> <seconds of wall time> [expect-text]

Runs the pure-Python reference (RP2040PY_SKIP_CYTHON) so the hooks see every access. `-I` keeps the working directory off sys.path. Needs stdin to be a pipe or a terminal (the console reader
registers it): `... < <(sleep 100)` from a script.
"""

import collections
import os
import signal
import sys

os.environ["RP2040PY_SKIP_CYTHON"] = "1"
from rp2040py.peripherals import _usb as U  # noqa: E402

NAMES = {
    getattr(U, name): name
    for name in (
        "ADDR_ENDP MAIN_CTRL SOF_WR SOF_RD SIE_CTRL SIE_STATUS INT_EP_CTRL BUFF_STATUS BUFF_CPU_SHOULD_HANDLE EP_ABORT EP_ABORT_DONE EP_STALL_ARM NAK_POLL EP_STATUS_STALL_NAK USB_MUXING "
        "USB_PWR USBPHY_DIRECT USBPHY_DIRECT_OVERRIDE USBPHY_TRIM INTR INTE INTF INTS"
    ).split()
}
HEAD = 400
log: collections.deque = collections.deque(maxlen=80)
head: list = []


def record(entry: tuple) -> None:
    log.append(entry)
    if len(head) < HEAD:
        head.append(entry)


def now(usb) -> int:
    return int(usb.rp2040.clock.ticks)  # 1/16 ns


read, write, dpram, done = U.RPUSBController.read_uint32, U.RPUSBController.write_uint32, U.RPUSBController.dpram_updated, U.RPUSBController._buffer_done


def traced_read(self, offset):
    value = read(self, offset)
    record(("R", NAMES.get(offset, hex(offset)), hex(value), now(self)))
    return value


def traced_write(self, offset, value):
    record(("W", NAMES.get(offset, hex(offset)), hex(value), now(self)))
    return write(self, offset, value)


def traced_dpram(self, offset, value):
    word = int.from_bytes(self.rp2040.usb_dpram[offset : offset + 4], "little")
    record(("D", hex(offset), hex(word), now(self)))
    return dpram(self, offset, value)


def traced_done(self, endpoint, out, half, received):
    record(("done", endpoint, "out" if out else "in", half, now(self)))
    return done(self, endpoint, out, half, received)


U.RPUSBController.read_uint32, U.RPUSBController.write_uint32 = traced_read, traced_write
U.RPUSBController.dpram_updated, U.RPUSBController._buffer_done = traced_dpram, traced_done


def dump(*_):
    print("--- first accesses")
    for entry in head:
        print(*entry)
    print("--- last accesses")
    for entry in log:
        print(*entry)
    sys.stdout.flush()
    os._exit(3)


if __name__ == "__main__":
    signal.signal(signal.SIGALRM, dump)
    signal.alarm(int(sys.argv[2]))
    from rp2040py.cli import main

    main(["micropython", "--image", sys.argv[1], "--expect-text", sys.argv[3] if len(sys.argv) > 3 else "Hello, world!"])
