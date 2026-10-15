"""The Cortex-M0+ system-control registers of the PPB (RP2040 datasheet 2.4.4 SCB, 2.4.6 MPU) through a whole chip, on whichever PPB the facade hands out.

The register-level behaviour is pinned by tests/test_ppb_diff.py (reference against the facade) and tests/cpp/test_ppb.cpp; this file is what needs a real chip around the block:
AIRCR.SYSRESETREQ's default handler ("Reset the core only", datasheet 2.4.2.9) and the SVCALL pending bit reaching the core.
"""

from rp2040py.rp2040 import RP2040

PPB = 0xE000E000
AIRCR, SCR, CCR, SHCSR, VTOR = 0xD0C, 0xD10, 0xD14, 0xD24, 0xD08
SYST_CSR, SYST_RVR = 0x010, 0x014


def test_sysresetreq_resets_the_core_and_not_the_chip():
    chip = RP2040()
    chip.core.registers[3] = 1234
    chip.core.pc = 0x20000100
    chip.write_uint32(PPB + SYST_RVR, 99)
    chip.write_uint32(PPB + SCR, 0x16)
    chip.write_uint32(PPB + AIRCR, 0x05FA0004)
    assert chip.core.registers[3] == 0
    assert chip.core.pc == 0x10000000  # leave_reset(): where the bootrom would have left it
    assert chip.read_uint32(PPB + SYST_RVR) == 0xFFFFFF  # the PPB is part of the core and resets with it
    assert chip.read_uint32(PPB + SCR) == 0


def test_sysresetreq_without_the_key_does_nothing():
    chip = RP2040()
    chip.core.registers[3] = 1234
    chip.write_uint32(PPB + AIRCR, 0x00000004)
    chip.write_uint32(PPB + AIRCR, 0x05FB0004)
    assert chip.core.registers[3] == 1234


def test_a_replaced_handler_is_called_instead_of_the_default():
    chip = RP2040()
    calls = []
    chip.ppb.on_system_reset_request = lambda: calls.append(1)
    chip.core.registers[3] = 1234
    chip.write_uint32(PPB + AIRCR, 0x05FA0004)
    assert calls == [1]
    assert chip.core.registers[3] == 1234


def test_reserved_bits_and_the_fixed_registers():
    chip = RP2040()
    assert chip.read_uint32(PPB + AIRCR) == 0xFA050000
    assert chip.read_uint32(PPB + CCR) == 0x208
    chip.write_uint32(PPB + CCR, 0)
    assert chip.read_uint32(PPB + CCR) == 0x208
    chip.write_uint32(PPB + VTOR, 0x200000FF)
    assert chip.read_uint32(PPB + VTOR) == 0x20000000
    assert chip.read_uint32(PPB + 0xD90) == 0x800  # MPU_TYPE: 8 regions


def test_svcallpended_reaches_the_core():
    chip = RP2040()
    chip.write_uint32(PPB + SHCSR, 1 << 15)
    assert chip.core.pending_svcall
    assert chip.read_uint32(PPB + SHCSR) == 1 << 15
    chip.write_uint32(PPB + SHCSR, 0)
    assert not chip.core.pending_svcall
