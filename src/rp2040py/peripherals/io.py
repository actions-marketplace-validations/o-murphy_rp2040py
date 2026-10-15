"""Public facade for IO_BANK0 / IO_QSPI: prefers the native block in `rp2040py.native._io` (a Cython shell over `native/core/io.hpp`, working on the C++ pin bank the native pins share) when it
is importable, falling back to the plain-Python reference in `_io.py` otherwise. Every caller imports `RPIO` from here - never from `_io.py` or `rp2040py.native` directly (the pure-Python chip,
`_rp2040.py`, is the one deliberate exception). Held to identical behaviour by `tests/test_pin_diff.py` (docs/records/0096-cpp-mcu-core.md).
"""

from rp2040py._native_gate import native_disabled

try:
    if native_disabled():
        raise ImportError("RP2040PY_SKIP_CYTHON=1 set, forcing pure-Python fallback")
    from rp2040py.native._io import RPIO
except ImportError:
    from rp2040py.peripherals._io import RPIO

__all__ = ("RPIO",)
