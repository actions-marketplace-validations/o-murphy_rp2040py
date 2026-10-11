"""CLOCKS and PLL register behaviour the datasheet pins down (docs/records/0098-datasheet-conformance-audit.md): which bits of the dividers exist, the reset values, the
widths of the PLL registers. Pure Python blocks: the same on both builds."""

CLOCKS = 0x40008000
PLL_SYS = 0x40028000


def test_clock_dividers_reset_to_one_and_ref_usb_adc_have_only_a_two_bit_integer_part(rp2040_factory):
    chip = rp2040_factory()
    for offset in (0x04, 0x10, 0x1C, 0x28, 0x34, 0x40, 0x58, 0x64, 0x70):  # GPOUT0-3, REF, SYS, USB, ADC, RTC: INT = 1
        assert chip.read_uint32(CLOCKS + offset) == 0x100, hex(offset)
    for offset in (0x34, 0x58, 0x64):  # CLK_REF_DIV, CLK_USB_DIV, CLK_ADC_DIV: INT is bits 9:8, the rest is reserved
        chip.write_uint32(CLOCKS + offset, 0xFFFFFFFF)
        assert chip.read_uint32(CLOCKS + offset) == 0x300, hex(offset)
    for offset in (0x04, 0x40, 0x70):  # the others are 24.8 fixed point
        chip.write_uint32(CLOCKS + offset, 0xFFFFFFFF)
        assert chip.read_uint32(CLOCKS + offset) == 0xFFFFFFFF, hex(offset)


def test_the_clocks_registers_that_are_stored_have_their_reset_values_and_widths(rp2040_factory):
    chip = rp2040_factory()
    assert chip.read_uint32(CLOCKS + 0x78) == 0xFF  # CLK_SYS_RESUS_CTRL: TIMEOUT resets to 0xFF
    assert chip.read_uint32(CLOCKS + 0xA0) == 0xFFFFFFFF and chip.read_uint32(CLOCKS + 0xA4) == 0x7FFF  # WAKE_EN0/1
    assert chip.read_uint32(CLOCKS + 0xA8) == 0xFFFFFFFF and chip.read_uint32(CLOCKS + 0xAC) == 0x7FFF  # SLEEP_EN0/1
    for offset, kept in ((0x78, 0x111FF), (0xA4, 0x7FFF), (0xAC, 0x7FFF), (0xBC, 0x1), (0xC0, 0x1)):
        chip.write_uint32(CLOCKS + offset, 0xFFFFFFFF)
        assert chip.read_uint32(CLOCKS + offset) == kept, hex(offset)
    assert chip.read_uint32(CLOCKS + 0xC4) == 1  # INTS = (INTR & INTE) | INTF


def test_pll_registers_keep_only_their_datasheet_bits(rp2040_factory):
    chip = rp2040_factory()
    assert chip.read_uint32(PLL_SYS + 0x0C) == 0x77000  # PRIM: POSTDIV1 = POSTDIV2 = 7
    for offset, kept in ((0x04, 0x2D), (0x08, 0xFFF), (0x0C, 0x77000)):
        chip.write_uint32(PLL_SYS + offset, 0xFFFFFFFF)
        assert chip.read_uint32(PLL_SYS + offset) == kept, hex(offset)
    chip.write_uint32(PLL_SYS + 0x00, 0xFFFFFFFF)  # CS: BYPASS 8 and REFDIV 5:0 (LOCK 31 is read only)
    assert chip.read_uint32(PLL_SYS + 0x00) == 0x80000000 | 0x13F


# ---- the frequency counter: datasheet 2.15.4, 2.15.6.2 and tables 238-246; pico-sdk `frequency_count_khz()` ----
FC0_REF_KHZ, FC0_MIN_KHZ, FC0_MAX_KHZ, FC0_DELAY, FC0_INTERVAL, FC0_SRC, FC0_STATUS, FC0_RESULT = (
    0x80,
    0x84,
    0x88,
    0x8C,
    0x90,
    0x94,
    0x98,
    0x9C,
)
RUNNING, DONE, PASS, FAIL, SLOW, FAST, DIED = 1 << 8, 1 << 4, 1, 1 << 16, 1 << 20, 1 << 24, 1 << 28
XOSC_SRC, PLL_SYS_SRC, PLL_USB_SRC, ROSC_SRC, CLK_REF_SRC, CLK_SYS_SRC, CLK_PERI_SRC, CLK_RTC_SRC, GPIN0_SRC = (
    5,
    1,
    2,
    3,
    8,
    9,
    10,
    13,
    6,
)


def _measure(chip, src, interval=10):
    """`frequency_count_khz()` without the busy-wait: the SDK's own setup, then the simulated time of the test (0.98 us * 2**interval)."""
    chip.write_uint32(CLOCKS + FC0_REF_KHZ, 12000)
    chip.write_uint32(CLOCKS + FC0_INTERVAL, interval)
    chip.write_uint32(CLOCKS + FC0_MIN_KHZ, 0)
    chip.write_uint32(CLOCKS + FC0_MAX_KHZ, 0xFFFFFFFF)
    chip.write_uint32(CLOCKS + FC0_SRC, src)
    assert chip.read_uint32(CLOCKS + FC0_STATUS) & RUNNING and not chip.read_uint32(CLOCKS + FC0_STATUS) & DONE
    chip.clock.tick(
        1e6 / 12000 + 980.0 * (1 << interval) + 1
    )  # FC0_DELAY (1) periods of the 12 MHz clk_ref, then the interval
    return chip.read_uint32(CLOCKS + FC0_STATUS), chip.read_uint32(CLOCKS + FC0_RESULT)


def test_the_frequency_counter_registers_have_their_widths_and_reset_values(rp2040_factory):
    chip = rp2040_factory()
    resets = {
        FC0_REF_KHZ: 0,
        FC0_MIN_KHZ: 0,
        FC0_MAX_KHZ: 0x1FFFFFF,
        FC0_DELAY: 1,
        FC0_INTERVAL: 8,
        FC0_SRC: 0,
        FC0_STATUS: 0,
        FC0_RESULT: 0,
    }
    for offset, reset in resets.items():
        assert chip.read_uint32(CLOCKS + offset) == reset, hex(offset)
    for offset, kept in (
        (FC0_REF_KHZ, 0xFFFFF),
        (FC0_MIN_KHZ, 0x1FFFFFF),
        (FC0_MAX_KHZ, 0x1FFFFFF),
        (FC0_DELAY, 7),
        (FC0_INTERVAL, 0xF),
    ):
        chip.write_uint32(CLOCKS + offset, 0xFFFFFFFF)
        assert chip.read_uint32(CLOCKS + offset) == kept, hex(offset)
    chip.write_uint32(CLOCKS + FC0_STATUS, 0xFFFFFFFF)  # read only
    chip.write_uint32(CLOCKS + FC0_RESULT, 0xFFFFFFFF)
    assert chip.read_uint32(CLOCKS + FC0_STATUS) == 0 and chip.read_uint32(CLOCKS + FC0_RESULT) == 0


def test_a_count_runs_for_the_interval_and_reports_the_frequency_in_khz_with_five_fraction_bits(rp2040_factory):
    chip = rp2040_factory()
    chip.write_uint32(CLOCKS + FC0_REF_KHZ, 12000)
    chip.write_uint32(CLOCKS + FC0_SRC, XOSC_SRC)  # the default interval, 8: 250 us
    chip.clock.tick(1e6 / 12000 + 250_880 - 1)  # one reference period of delay, then 0.98 us * 2**8
    assert chip.read_uint32(CLOCKS + FC0_STATUS) == RUNNING
    chip.clock.tick(2)
    assert chip.read_uint32(CLOCKS + FC0_STATUS) == DONE | PASS
    assert chip.read_uint32(CLOCKS + FC0_RESULT) == 12000 << 5  # 12 MHz, no fraction
    status, result = _measure(chip, ROSC_SRC)
    assert status == DONE | PASS and result >> 5 == 6500 and (result & 31) == 0  # the ring oscillator's nominal 6.5 MHz
    chip.write_uint32(CLOCKS + 0x6C, (1 << 11) | (3 << 5))  # CLK_RTC from XOSC, divided by 256 ...
    chip.write_uint32(CLOCKS + 0x70, 256 << 8)
    status, result = _measure(chip, CLK_RTC_SRC)
    assert (
        status == DONE | PASS and result >> 5 == 46 and (result & 31) == int((46.875 - 46) * 32)
    )  # 46.875 kHz: KHZ 46, FRAC 28/32


def test_every_source_of_the_counter_reads_what_the_clock_tree_says(rp2040_factory):
    chip = rp2040_factory()
    assert _measure(chip, PLL_SYS_SRC)[1] == 0 and _measure(chip, PLL_SYS_SRC)[0] & DIED  # a PLL that is not running
    assert _measure(chip, CLK_SYS_SRC)[1] >> 5 == 125_000  # 125 MHz until firmware configures the tree
    assert _measure(chip, CLK_PERI_SRC)[1] >> 5 == 125_000
    assert _measure(chip, CLK_REF_SRC)[1] >> 5 == 6500  # clk_ref from the ring oscillator at reset
    assert _measure(chip, XOSC_SRC)[1] >> 5 == 12_000
    P = 0x4002C000  # PLL_USB as pico-sdk configures it: 48 MHz
    chip.write_uint32(P + 0x00, 1)
    chip.write_uint32(P + 0x08, 100)
    chip.write_uint32(P + 0x0C, (5 << 16) | (5 << 12))
    chip.write_uint32(P + 0x04, 0)
    assert _measure(chip, PLL_USB_SRC)[1] >> 5 == 48_000
    chip.write_uint32(CLOCKS + 0x54, (1 << 11) | 0)  # CLK_USB_CTRL: enabled, AUXSRC PLL_USB, divider 1
    assert _measure(chip, 0x0B)[1] >> 5 == 48_000
    chip.write_uint32(CLOCKS + 0x60, (1 << 11) | (3 << 5))  # CLK_ADC_CTRL from XOSC
    assert _measure(chip, 0x0C)[1] >> 5 == 12_000


def test_a_stopped_clock_a_null_source_and_gpin_set_died_and_fail_but_the_count_still_completes(rp2040_factory):
    chip = rp2040_factory()
    for src in (0, GPIN0_SRC, 0x7F):  # NULL, a GPIO clock (not modelled), a value that selects nothing
        status, result = _measure(chip, src)
        assert status == DONE | DIED | FAIL and result == 0, hex(src)
    assert not _measure(chip, XOSC_SRC)[0] & (DIED | FAIL)


def test_min_and_max_make_the_counter_a_pass_fail_test(rp2040_factory):
    chip = rp2040_factory()
    chip.write_uint32(CLOCKS + FC0_REF_KHZ, 12000)

    def run(minimum, maximum):
        chip.write_uint32(CLOCKS + FC0_MIN_KHZ, minimum)
        chip.write_uint32(CLOCKS + FC0_MAX_KHZ, maximum)
        chip.write_uint32(CLOCKS + FC0_SRC, XOSC_SRC)
        chip.clock.tick(300_000)
        return chip.read_uint32(CLOCKS + FC0_STATUS)

    assert run(11_000, 13_000) == DONE | PASS
    assert run(13_000, 20_000) == DONE | SLOW | FAIL  # 12 MHz is slower than the minimum
    assert run(0, 11_000) == DONE | FAST | FAIL


def test_a_new_count_replaces_the_one_in_progress_and_a_reset_clears_the_counter(rp2040_factory):
    chip = rp2040_factory()
    chip.write_uint32(CLOCKS + FC0_REF_KHZ, 12000)
    chip.write_uint32(CLOCKS + FC0_SRC, XOSC_SRC)
    chip.clock.tick(100_000)
    chip.write_uint32(CLOCKS + FC0_SRC, ROSC_SRC)  # restarted: the 250 us run again from here
    chip.clock.tick(200_000)
    assert chip.read_uint32(CLOCKS + FC0_STATUS) == RUNNING
    chip.clock.tick(100_000)
    assert chip.read_uint32(CLOCKS + FC0_RESULT) >> 5 == 6500
    chip.clocks.reset()  # RESETS.RESET_CLOCKS
    assert [
        chip.read_uint32(CLOCKS + o)
        for o in (FC0_REF_KHZ, FC0_MAX_KHZ, FC0_DELAY, FC0_INTERVAL, FC0_SRC, FC0_STATUS, FC0_RESULT)
    ] == [0, 0x1FFFFFF, 1, 8, 0, 0, 0]
