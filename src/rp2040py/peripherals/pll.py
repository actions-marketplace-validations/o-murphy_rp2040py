"""Public facade for the two PLLs: re-exports the plain-Python reference in `_pll.py` (see `clocks.py`; the C++ block is `native/core/pll.hpp`, not built yet - docs/tasks/clocks-port.md)."""

from rp2040py.peripherals._pll import RPPLL

__all__ = ("RPPLL",)
