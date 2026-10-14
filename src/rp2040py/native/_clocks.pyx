# cython: language_level=3, boundscheck=False, wraparound=False, cdivision=True, freethreading_compatible=True
"""The native RP2040 CLOCKS block and clock tree: a Python-facing shell over the C++ `ClocksBlock` of `core/clocks.hpp` (docs/records/0096-cpp-mcu-core.md, docs/tasks/clocks-port.md).
`peripherals/_clocks.py` is the pure-Python reference, kept as the oracle; `peripherals/clocks.py` is the facade that picks between them.

What a firmware sees is the C++ block: when the chip adopts this object it registers the block's own C++ read/write functions in the bus's window table (see `_native_window`), so a register
access never enters Python. The tree itself - the registers, the frequency of each generator, `clk_sys` / `clk_peri` and the frequency counter - is C++. What stays Python is the edge: the logger,
the oscillators' output (the XOSC block's STABLE bit and the chip's `xosc_freq`, the chip's `rosc_freq`) and the four SINKS the tree tells of a change, in the order of the reference's `update_clocks()`:
sys (the chip's `clk_sys`, then SysTick, then every PWM counter, then the host's listeners), peri (the chip's `clk_peri`, then every UART), ref (the watchdog's tick) and rtc (the RTC). Wiring those
to the C++ blocks directly is the step that lets a headless core run the tree without Python (docs/tasks/clocks-port.md). Failures of the Python side are parked in the shared slot of
`_pending.pyx` and re-raised by whoever called in. The frequency counter counts against the chip's C++ `Clock`, so the block needs the native `SimulationClock`.
"""

from libc.stdint cimport int64_t, uint32_t
from libcpp cimport bool as cppbool

from rp2040py.native._pending cimport park_error, pending_flag, raise_if_pending
from rp2040py.native._pll cimport RPPLL
from rp2040py.native._simulation_clock cimport SimulationClock
from rp2040py.native._window_map cimport WindowHandler

__all__ = ("RPClocks",)

XOSC_STATUS = 0x04
XOSC_STATUS_STABLE = 1 << 31


cdef void _warn_trampoline(void* ctx, uint32_t kind, uint32_t offset, int64_t value) noexcept:
    cdef RPClocks block = <RPClocks> ctx
    try:
        if kind == kClocksWarnRead:
            block.warn(f"Unimplemented peripheral read from 0x{offset:x}")
        elif kind == kClocksWarnReadAtomicArea:
            block.warn("Unimplemented read from peripheral in the atomic operation region")
        else:
            block.warn(f"Unimplemented peripheral write to 0x{offset:x}: 0x{value:x}")
    except BaseException as error:
        park_error(error)


cdef double _xosc_trampoline(void* ctx) noexcept:
    cdef RPClocks block = <RPClocks> ctx
    try:
        rp2040 = block.rp2040
        return <double> rp2040.xosc_freq if rp2040.xosc.read_uint32(XOSC_STATUS) & XOSC_STATUS_STABLE else 0.0
    except BaseException as error:
        park_error(error)
        return 0.0


cdef double _rosc_trampoline(void* ctx) noexcept:
    cdef RPClocks block = <RPClocks> ctx
    try:
        return <double> block.rp2040.rosc_freq
    except BaseException as error:
        park_error(error)
        return 0.0


cdef cppbool _sys_sink(void* ctx, double hz, double old) noexcept:
    cdef RPClocks block = <RPClocks> ctx
    try:
        rp2040 = block.rp2040
        rp2040.clk_sys = hz
        block._sys_dirty = 1
        rp2040.ppb.clk_sys_changed(hz)
        pwm = rp2040.pwm
        for channel in pwm.channels:
            channel.timer.frequency = pwm.clock_freq
        for listener in list(rp2040._clock_listeners):
            listener(hz, old)
    except BaseException as error:
        park_error(error)
        return False
    return True


cdef cppbool _peri_sink(void* ctx, double hz, double old) noexcept:
    cdef RPClocks block = <RPClocks> ctx
    try:
        rp2040 = block.rp2040
        rp2040.clk_peri = hz
        for uart in rp2040.uart:
            uart.clk_peri_changed()
    except BaseException as error:
        park_error(error)
        return False
    return True


cdef cppbool _ref_sink(void* ctx, double hz, double old) noexcept:
    cdef RPClocks block = <RPClocks> ctx
    try:
        block.rp2040.watchdog.clk_ref_changed(hz)
    except BaseException as error:
        park_error(error)
        return False
    return True


cdef cppbool _rtc_sink(void* ctx, double hz, double old) noexcept:
    cdef RPClocks block = <RPClocks> ctx
    try:
        block.rp2040.rtc.clk_rtc_changed(hz)
    except BaseException as error:
        park_error(error)
        return False
    return True


cdef class RPClocks:
    def __init__(self, rp2040, name):
        cdef ClocksHost host
        pll_sys = rp2040.pll_sys  # the chip builds the PLLs first: the tree points at their C++ blocks
        pll_usb = rp2040.pll_usb
        clock = rp2040.clock
        if not isinstance(clock, SimulationClock):
            raise TypeError(
                "the native CLOCKS block counts its frequency counter against the native SimulationClock; "
                f"{type(rp2040).__name__} with {type(clock).__name__} is not that "
                "(use the pure-Python peripherals/_clocks.RPClocks with it)"
            )
        if not isinstance(pll_sys, RPPLL) or not isinstance(pll_usb, RPPLL):
            raise TypeError("the native CLOCKS block reads the native PLLs (peripherals/_pll.RPPLL needs the pure-Python RPClocks)")
        self.rp2040 = rp2040
        self.name = name
        self.clock = clock
        self._pll_sys = pll_sys
        self._pll_usb = pll_usb
        host.warn = _warn_trampoline
        host.xosc_hz = _xosc_trampoline
        host.rosc_hz = _rosc_trampoline
        host.ctx = <void*> self
        host.failed = pending_flag()
        self._block.init(&(<SimulationClock> clock)._clock, host, &(<RPPLL> pll_sys)._block, &(<RPPLL> pll_usb)._block)
        if not (
            self._block.add_sys_sink(_sys_sink, <void*> self)
            and self._block.add_peri_sink(_peri_sink, <void*> self)
            and self._block.add_ref_sink(_ref_sink, <void*> self)
            and self._block.add_rtc_sink(_rtc_sink, <void*> self)
        ):
            raise RuntimeError("the clock tree's sink lists are full")

    @property
    def raw_write_value(self):
        return self._block.raw_write_value()

    def read_uint32(self, offset):
        cdef uint32_t value = self._block.read(<uint32_t> offset)
        raise_if_pending()
        return value

    def write_uint32(self, offset, value):
        if not self._block.write(<uint32_t> offset, <int64_t> value):
            raise_if_pending()

    def write_uint32_atomic(self, offset, value, atomic_type):
        if not self._block.write_atomic(<uint32_t> offset, <int64_t> value, <uint32_t> atomic_type):
            raise_if_pending()
        raise_if_pending()

    def reset(self):
        """The CLOCKS registers back to power-on (the clock tree's `clk_sys` / `clk_peri` are `reset_tree()`'s)."""
        self._block.reset()

    def reset_tree(self):
        """The CLOCKS domain was reset (after `reset()`): both PLLs to power-on, clk_sys / clk_peri back to 125 MHz through the sinks."""
        if not self._block.reset_tree():
            raise_if_pending()

    def update(self):
        """Re-derives `clk_sys` / `clk_peri` and retunes what runs from them (the chip's `update_clocks()`)."""
        if not self._block.retune():
            raise_if_pending()
        raise_if_pending()

    @property
    def ref_freq(self):
        value = self._block.ref_freq()
        raise_if_pending()
        return value

    @property
    def sys_freq(self):
        value = self._block.sys_freq()
        raise_if_pending()
        return value

    @property
    def peri_freq(self):
        value = self._block.peri_freq()
        raise_if_pending()
        return value

    @property
    def rtc_freq(self):
        value = self._block.rtc_freq()
        raise_if_pending()
        return value

    @property
    def usb_freq(self):
        value = self._block.usb_freq()
        raise_if_pending()
        return value

    @property
    def adc_freq(self):
        value = self._block.adc_freq()
        raise_if_pending()
        return value

    def frequency_of(self, source):
        value = self._block.frequency_of(<uint32_t> source)
        raise_if_pending()
        return value

    @property
    def clk_sys(self):
        return self._block.clk_sys()

    @property
    def clk_peri(self):
        return self._block.clk_peri()

    @property
    def ref_ctrl(self):
        return self._block.ref_ctrl

    @ref_ctrl.setter
    def ref_ctrl(self, value):
        self._block.ref_ctrl = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def ref_div(self):
        return self._block.ref_div

    @ref_div.setter
    def ref_div(self, value):
        self._block.ref_div = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def sys_ctrl(self):
        return self._block.sys_ctrl

    @sys_ctrl.setter
    def sys_ctrl(self, value):
        self._block.sys_ctrl = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def sys_div(self):
        return self._block.sys_div

    @sys_div.setter
    def sys_div(self, value):
        self._block.sys_div = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def peri_ctrl(self):
        return self._block.peri_ctrl

    @peri_ctrl.setter
    def peri_ctrl(self, value):
        self._block.peri_ctrl = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def peri_div(self):
        return self._block.peri_div

    @peri_div.setter
    def peri_div(self, value):
        self._block.peri_div = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def usb_ctrl(self):
        return self._block.usb_ctrl

    @usb_ctrl.setter
    def usb_ctrl(self, value):
        self._block.usb_ctrl = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def usb_div(self):
        return self._block.usb_div

    @usb_div.setter
    def usb_div(self, value):
        self._block.usb_div = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def adc_ctrl(self):
        return self._block.adc_ctrl

    @adc_ctrl.setter
    def adc_ctrl(self, value):
        self._block.adc_ctrl = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def adc_div(self):
        return self._block.adc_div

    @adc_div.setter
    def adc_div(self, value):
        self._block.adc_div = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def rtc_ctrl(self):
        return self._block.rtc_ctrl

    @rtc_ctrl.setter
    def rtc_ctrl(self, value):
        self._block.rtc_ctrl = <uint32_t> (value & 0xFFFFFFFF)

    @property
    def rtc_div(self):
        return self._block.rtc_div

    @rtc_div.setter
    def rtc_div(self, value):
        self._block.rtc_div = <uint32_t> (value & 0xFFFFFFFF)

    def __dealloc__(self):
        # Off the clock's alarm list before the block goes away (a no-op if the clock is gone already).
        self._block.detach()

    def debug(self, msg):
        self.rp2040.logger.debug(self.name, msg)

    def info(self, msg):
        self.rp2040.logger.info(self.name, msg)

    def warn(self, msg):
        self.rp2040.logger.warning(self.name, msg)

    def error(self, msg):
        self.rp2040.logger.error(self.name, msg)

    # --- the native bus protocol -----------------------------------------------------------------

    def _native_window(self):
        """What `RP2040` registers in its C++ window table when it adopts this block, instead of a Python trampoline: the addresses of the block's
        own read/write functions and its context. Looked up on the *type* by the bus, so a wrapper that merely forwards attributes (a recorder,
        a profiler) cannot lend its target's fast path and be bypassed."""
        cdef WindowHandler handler = self._block.window_handler()
        return (<size_t> handler.read32, <size_t> handler.write32, <size_t> handler.ctx)
