"""The two `RP2040` implementations side by side, for parity tests (docs/records/0096-cpp-mcu-core.md).

Skips the importing test module when the native extension is not built. `make_chip(PurePython | Native, ...)`
returns a chip with a silent logger, an optional boot ROM image, and the USB-DPRAM hook recorded in
`chip.hook_calls`.
"""

import pytest

pytest.importorskip("rp2040py.native._rp2040", reason="the native extension is not built")

from rp2040py._rp2040 import RP2040 as PurePython
from rp2040py.native._rp2040 import RP2040 as Native

__all__ = ("Native", "PurePython", "make_chip")


class _Quiet:
    """A logger that says nothing: the two buses warn identically on unmapped/unaligned accesses."""

    def __getattr__(self, name):
        return lambda *args, **kwargs: None


def make_chip(cls, bootrom_words=None):
    chip = cls()
    chip.logger = _Quiet()
    if bootrom_words is not None:
        chip.load_bootrom(bootrom_words)
    chip.hook_calls = []
    chip.usb_ctrl.dpram_updated = lambda offset, value: chip.hook_calls.append((offset, value))
    return chip
