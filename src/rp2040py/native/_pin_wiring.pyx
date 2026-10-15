# cython: language_level=3, boundscheck=False, wraparound=False, cdivision=True, freethreading_compatible=True
"""Wires the sources of a pin's level straight to the registers of the blocks that drive it (docs/records/0096-cpp-mcu-core.md #29).

A pin's level is a function of its own state and of other blocks' registers: SIO's output enable / value, each PIO's pin directions / values, the PWM's. Through the pin bank's host those
are read by a Python trampoline, attribute by attribute; when the block is the native one, the bank can instead be given the address of the C++ register itself, and a level evaluation
never leaves C++. This module is the one place that knows all four block types (the pin module itself cannot cimport them: the PIO cimports the pin).

`bind_pin_sources(bank, chip)` is idempotent and can be called again whenever the chip's blocks are replaced (a source whose block is not the native one goes back to the host, so a stand-in
keeps working). The bank keeps the blocks alive for as long as it can read them.
"""

from rp2040py.native._gpio_pin cimport GPIOPinBank
from rp2040py.native._pin cimport (
    PinBank,
    kSrcPio0Oe,
    kSrcPio0Value,
    kSrcPio1Oe,
    kSrcPio1Value,
    kSrcPwmDirection,
    kSrcPwmValue,
    kSrcSioOe,
    kSrcSioQspiOe,
    kSrcSioQspiValue,
    kSrcSioValue,
)
from rp2040py.native._pio cimport RPPIO
from rp2040py.native._pwm cimport RPPWM
from rp2040py.native._sio cimport RPSIO

__all__ = ("bind_pin_sources",)


cdef _keep(GPIOPinBank bank, block):
    if not any(block is kept for kept in bank._direct_refs):
        bank._direct_refs.append(block)


def bind_pin_sources(GPIOPinBank bank, chip):
    """Binds the bank's sources to the native blocks of `chip` that have them, and hands the rest back to the host."""
    cdef PinBank* b = bank._bank
    cdef RPSIO sio
    cdef RPPIO pio
    cdef RPPWM pwm
    sio_block = getattr(chip, "sio", None)
    if isinstance(sio_block, RPSIO):
        sio = <RPSIO> sio_block
        b.bind_source(kSrcSioOe, &sio._block.gpio_output_enable)
        b.bind_source(kSrcSioValue, &sio._block.gpio_value)
        b.bind_source(kSrcSioQspiOe, &sio._block.qspi_gpio_output_enable)
        b.bind_source(kSrcSioQspiValue, &sio._block.qspi_gpio_value)
        _keep(bank, sio)
    else:
        b.bind_source(kSrcSioOe, NULL)
        b.bind_source(kSrcSioValue, NULL)
        b.bind_source(kSrcSioQspiOe, NULL)
        b.bind_source(kSrcSioQspiValue, NULL)
    pios = getattr(chip, "pio", None)
    for index, (oe, value) in enumerate(((kSrcPio0Oe, kSrcPio0Value), (kSrcPio1Oe, kSrcPio1Value))):
        if pios is not None and len(pios) > index and isinstance(pios[index], RPPIO):
            pio = <RPPIO> pios[index]
            b.bind_source(oe, &pio._block.pin_directions)
            b.bind_source(value, &pio._block.pin_values)
            _keep(bank, pio)
        else:
            b.bind_source(oe, NULL)
            b.bind_source(value, NULL)
    pwm_block = getattr(chip, "pwm", None)
    if isinstance(pwm_block, RPPWM):
        pwm = <RPPWM> pwm_block
        b.bind_source(kSrcPwmDirection, &pwm._block.gpio_direction)
        b.bind_source(kSrcPwmValue, &pwm._block.gpio_value)
        _keep(bank, pwm)
    else:
        b.bind_source(kSrcPwmDirection, NULL)
        b.bind_source(kSrcPwmValue, NULL)
