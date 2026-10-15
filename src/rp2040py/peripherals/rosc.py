"""Public facade for the ROSC: prefers the native block in `rp2040py.native._rosc` (a Cython shell over `native/core/rosc.hpp`) when it is importable, falling back to the plain-Python
reference in `_rosc.py` otherwise. Every caller imports `RPROSC` from here - never from `_rosc.py` or `rp2040py.native` directly (the pure-Python chip, `_rp2040.py`, is the one deliberate
exception). Mirrors `peripherals/xosc.py`; held to identical behaviour by `tests/test_rosc_diff.py` (docs/records/0096-cpp-mcu-core.md).
"""

from rp2040py._native_gate import native_disabled

try:
    if native_disabled():
        raise ImportError("RP2040PY_SKIP_CYTHON=1 set, forcing pure-Python fallback")
    from rp2040py.native._rosc import RPROSC
except ImportError:
    from rp2040py.peripherals._rosc import RPROSC

__all__ = ("RPROSC",)
