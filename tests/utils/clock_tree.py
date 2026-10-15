"""Sets `clk_sys` the way firmware does - through the crystal, PLL_SYS and the CLOCKS registers - for tests that need the chip to run at a given frequency (a stand-in for pico-sdk's `set_sys_clock_khz()`)."""

from typing import Any

XOSC_HZ = 12e6
XOSC_CTRL = 0x40024000
PLL_SYS = 0x40028000
CLOCKS = 0x40008000
CLK_REF_CTRL, CLK_SYS_CTRL = CLOCKS + 0x30, CLOCKS + 0x3C


def pll_settings(hz: float) -> "tuple[int, int, int]":
    """(FBDIV, POSTDIV1, POSTDIV2) giving exactly `hz` from the 12 MHz crystal with a VCO in the design's 750-1600 MHz range, as `set_sys_clock_khz()` searches for them."""
    for post1 in range(7, 0, -1):
        for post2 in range(post1, 0, -1):
            for fbdiv in range(16, 321):
                vco = XOSC_HZ * fbdiv
                if 750e6 <= vco <= 1600e6 and vco / (post1 * post2) == hz:
                    return fbdiv, post1, post2
    raise ValueError(f"no PLL setting gives exactly {hz} Hz from a 12 MHz crystal")


def set_clk_sys(chip: Any, hz: float) -> None:
    chip.write_uint32(XOSC_CTRL, (0xFAB << 12) | 0xAA0)  # xosc_init()
    chip.write_uint32(CLK_REF_CTRL, 2)  # clk_ref from the crystal
    if hz == XOSC_HZ:
        chip.write_uint32(CLK_SYS_CTRL, 1 | (3 << 5))  # clk_sys straight from the crystal (aux source XOSC)
        return
    fbdiv, post1, post2 = pll_settings(hz)
    chip.write_uint32(PLL_SYS + 0x00, 1)  # REFDIV = 1
    chip.write_uint32(PLL_SYS + 0x08, fbdiv)
    chip.write_uint32(PLL_SYS + 0x04, 0)  # PD, VCOPD and POSTDIVPD cleared
    chip.write_uint32(PLL_SYS + 0x0C, (post1 << 16) | (post2 << 12))
    chip.write_uint32(CLK_SYS_CTRL, 1 | (0 << 5))  # clk_sys from its aux source, PLL_SYS
    assert chip.clk_sys == hz, (chip.clk_sys, hz)
