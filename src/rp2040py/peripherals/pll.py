"""Public facade for the two PLLs: prefers the native block in `rp2040py.native._pll` (a Cython shell over `native/core/pll.hpp`) when it is importable, falling back to the plain-Python reference in
`_pll.py` otherwise. Every caller imports `RPPLL` from here - never from `_pll.py` or `rp2040py.native` directly (the pure-Python chip, `_rp2040.py`, is the one deliberate exception).
Held to identical behaviour by `tests/test_clocks_diff.py` (docs/tasks/clocks-port.md).
"""

from rp2040py._native_gate import native_disabled

try:
    if native_disabled():
        raise ImportError("RP2040PY_SKIP_CYTHON=1 set, forcing pure-Python fallback")
    from rp2040py.native._pll import RPPLL
except ImportError:
    from rp2040py.peripherals._pll import RPPLL

__all__ = ("RPPLL",)
