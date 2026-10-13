"""Public facade for CLOCKS and the clock tree: prefers the native block in `rp2040py.native._clocks` (a Cython shell over `native/core/clocks.hpp`) when it is importable, falling back to the
plain-Python reference in `_clocks.py` otherwise. Every caller imports from here - never from `_clocks.py` or `rp2040py.native` directly (the pure-Python chip, `_rp2040.py`, is the one deliberate
exception: it keeps the reference as its oracle). The native block needs the native PLLs (`peripherals/pll.py`) and the native clock.
Held to identical behaviour by `tests/test_clocks_diff.py` (docs/tasks/clocks-port.md).
"""

from rp2040py._native_gate import native_disabled
from rp2040py.peripherals._clocks import DEFAULT_CLK, _set_clk_peri, _set_clk_sys, reset_clock_tree, update_clocks, xosc_hz

try:
    if native_disabled():
        raise ImportError("RP2040PY_SKIP_CYTHON=1 set, forcing pure-Python fallback")
    from rp2040py.native._clocks import RPClocks
except ImportError:
    from rp2040py.peripherals._clocks import RPClocks

__all__ = ("DEFAULT_CLK", "RPClocks", "_set_clk_peri", "_set_clk_sys", "reset_clock_tree", "update_clocks", "xosc_hz")
