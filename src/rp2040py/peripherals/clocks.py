"""Public facade for CLOCKS and the clock tree: re-exports the plain-Python reference in `_clocks.py`. Every caller imports from here - never from `_clocks.py` directly (the pure-Python chip,
`_rp2040.py`, is the one deliberate exception once there is a native twin). The C++ tree (`native/core/clocks.hpp`) is the next step of docs/tasks/clocks-port.md; until it exists this module has
nothing to prefer, and the facade only fixes the import path the port will switch behind.
"""

from rp2040py.peripherals._clocks import (
    DEFAULT_CLK,
    RPClocks,
    _set_clk_peri,
    _set_clk_sys,
    reset_clock_tree,
    update_clocks,
    xosc_hz,
)

__all__ = ("DEFAULT_CLK", "RPClocks", "_set_clk_peri", "_set_clk_sys", "reset_clock_tree", "update_clocks", "xosc_hz")
