# Declaration file paired with _gpio_pin.pyx. See that file's module docstring for the port
# rationale.
#
# native/_pio.pyx cimports GPIOPin from here so RPPIO.check_changed_pins() can call
# check_for_updates() as a direct C call (once per changed pin, millions of times during a CYW43
# boot) instead of an object-typed method dispatch.
#
# The pins' state lives in a C++ `PinBank` (core/pin.hpp), shared by a chip's pins (`GPIOPinBank`); a pin on its own gets a bank of one. Each
# `GPIOPin` is a view onto one slot of it (the bank, its slot and a pointer to the C++ pin).

from rp2040py.native._mcu_irq cimport McuIoIrq
from rp2040py.native._pin cimport Pin, PinBank, PinChangeFn

cdef class GPIOPinBank:
    cdef PinBank _bank
    cdef McuIoIrq _io_irq  # IO_BANK0's line straight to the chip, for the chip's 30 GPIO pins (core/mcu.hpp)
    cdef public object rp2040
    cdef public list shells
    cdef public list _direct_refs
    cdef public unsigned int first


cdef class GPIOPin:
    cdef public object rp2040
    cdef public str name
    cdef public object _listeners
    # Python objects a C++ consumer registered on this pin points into (the shifter itself, the PIO/SIO objects whose registers it reads
    # directly): the pin keeps them alive for as long as it can call them.
    cdef public list _direct_refs
    cdef GPIOPinBank _owner
    cdef PinBank* _bankp
    cdef public unsigned int _slot
    cdef Pin* _pin

    cpdef check_for_updates(self)

    # For C++-backed consumers that must answer in the same cycle (the CYW43 gSPI shifter): a direct listener is a function
    # pointer called from check_for_updates() before the Python listeners, so it never crosses into Python; `bank_ptr()` is the
    # pin's own C++ state, to read its level and drive its input from C++.
    cdef bint add_direct_listener(self, PinChangeFn fn, void* ctx)
    cdef bint remove_direct_listener(self, PinChangeFn fn, void* ctx)
    cdef PinBank* bank_ptr(self)
