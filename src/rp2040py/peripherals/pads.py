"""Public facade for PADS_BANK0 / PADS_QSPI: prefers the native block in `rp2040py.native._pads` (a Cython shell over `native/core/pads.hpp`, working on the C++ pin bank the native pins share)
when it is importable, falling back to the plain-Python reference in `_pads.py` otherwise. Every caller imports `RPPADS` (and `IIOBank`) from here - never from `_pads.py` or `rp2040py.native`
directly (the pure-Python chip, `_rp2040.py`, is the one deliberate exception). Held to identical behaviour by `tests/test_pin_diff.py` (docs/records/0096-cpp-mcu-core.md).
"""

from rp2040py._native_gate import native_disabled
from rp2040py.peripherals._pads import IIOBank

try:
    if native_disabled():
        raise ImportError("RP2040PY_SKIP_CYTHON=1 set, forcing pure-Python fallback")
    from rp2040py.native._pads import RPPADS
except ImportError:
    from rp2040py.peripherals._pads import RPPADS

__all__ = ("RPPADS", "IIOBank")
