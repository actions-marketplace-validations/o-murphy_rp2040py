"""IO_BANK0, IO_QSPI, PADS_BANK0 through a whole chip, on whichever pins the build selects (RP2040 datasheet 2.19, pico-sdk `io_bank0.h` / `pads_bank0.h`).

What these pin down was wrong before the audit of 0098's "IO_BANK0 / PADS_BANK0" section: the pad reset (`0x56`, not rp2040js's `0x36`), reserved bits that stuck, INTR's level bits (read only, they follow
the pin at all times - not only after the first input change) and its edges (they latch on a *transition*), the missing PROC1 / DORMANT_WAKE interrupt registers, IO_QSPI's own register layout, the SWCLK / SWD pads,
and a QSPI pin driven by SIO taking its level from `GPIO_HI_OUT` (not from `GPIO_OUT`).
"""

from rp2040py.gpio_pin import GPIOPinState

IO = 0x40014000
IO_QSPI = 0x40018000
PADS = 0x4001C000
SIO = 0xD0000000
GPIO_HI_OUT, GPIO_HI_OE = 0x30, 0x40
INTR0, PROC0_INTE0, PROC0_INTF0, PROC0_INTS0 = 0xF0, 0x100, 0x110, 0x120
PROC1_INTE0, PROC1_INTS0, DORMANT_INTE0, DORMANT_INTS0 = 0x130, 0x150, 0x160, 0x180
LEVEL_LOW, LEVEL_HIGH, EDGE_LOW, EDGE_HIGH = 1, 2, 4, 8
FUNCSEL_SIO = 5


def test_pad_reset_values_and_reserved_bits(rp2040_factory):
    chip = rp2040_factory()
    for pin in range(30):
        assert chip.read_uint32(PADS + 4 + 4 * pin) == 0x56  # OD 0, IE 1, DRIVE 4 mA, PDE 1, SCHMITT 1
    assert chip.read_uint32(PADS + 0x7C) == 0xDA  # SWCLK
    assert chip.read_uint32(PADS + 0x80) == 0x5A  # SWD
    chip.write_uint32(PADS + 4, 0xFFFFFFFF)
    chip.write_uint32(PADS + 0x7C, 0xFFFFFFFF)
    assert chip.read_uint32(PADS + 4) == 0xFF and chip.read_uint32(PADS + 0x7C) == 0xFF


def test_ctrl_keeps_only_its_defined_fields(rp2040_factory):
    chip = rp2040_factory()
    assert chip.read_uint32(IO + 4) == 0x1F  # FUNCSEL 31 = NULL
    chip.write_uint32(IO + 4, 0xFFFFFFFF)
    assert chip.read_uint32(IO + 4) == 0x3003331F  # IRQOVER, INOVER, OEOVER, OUTOVER, FUNCSEL


def test_level_bits_are_there_from_boot_and_follow_the_pin(rp2040_factory):
    chip = rp2040_factory()
    assert (
        chip.read_uint32(IO + INTR0) == 0x11111111
    )  # every pin reads low behind its pull-down and an enabled input: LEVEL_LOW
    assert chip.read_uint32(IO + INTR0 + 12) == 0x00111111  # GPIO24..29
    chip.gpio[0].set_input_value(True)
    assert chip.read_uint32(IO + INTR0) & 0xF == LEVEL_HIGH | EDGE_HIGH
    chip.gpio[0].set_input_value(False)
    assert chip.read_uint32(IO + INTR0) & 0xF == LEVEL_LOW | EDGE_LOW | EDGE_HIGH


def test_an_edge_needs_a_transition(rp2040_factory):
    chip = rp2040_factory()
    chip.gpio[2].set_input_value(False)  # already low: no falling edge
    assert (chip.read_uint32(IO + INTR0) >> 8) & 0xF == LEVEL_LOW
    chip.gpio[2].set_input_value(True)
    chip.write_uint32(IO + INTR0, 0xFFFFFFFF)  # EDGE bits are write-1-to-clear, the level bits are read only
    assert (chip.read_uint32(IO + INTR0) >> 8) & 0xF == LEVEL_HIGH
    chip.gpio[2].set_input_value(True)  # high again: still no new edge
    assert (chip.read_uint32(IO + INTR0) >> 8) & 0xF == LEVEL_HIGH


def test_the_input_buffer_gates_what_the_interrupt_sees(rp2040_factory):
    chip = rp2040_factory()
    chip.gpio[1].set_input_value(True)
    chip.write_uint32(IO + INTR0, 0xFFFFFFFF)
    chip.write_uint32(PADS + 8, 0x56 & ~0x40)  # IE off: the pin now reads low to the interrupt logic
    assert (chip.read_uint32(IO + INTR0) >> 4) & 0xF == LEVEL_LOW | EDGE_LOW
    chip.write_uint32(PADS + 8, 0x56)  # IE on: high again
    assert (chip.read_uint32(IO + INTR0) >> 4) & 0xF == LEVEL_HIGH | EDGE_LOW | EDGE_HIGH


def test_a_pull_on_a_floating_pin_is_an_input_change(rp2040_factory):
    chip = rp2040_factory()
    assert chip.read_uint32(IO + INTR0 + 4) & 0xF == LEVEL_LOW  # GPIO8 floats, pulled down
    chip.write_uint32(PADS + 4 + 4 * 8, 0x56 & ~0x4 | 0x8)  # PUE on, PDE off: it floats high
    assert chip.read_uint32(IO + INTR0 + 4) & 0xF == LEVEL_HIGH | EDGE_HIGH


def test_the_three_interrupt_destinations_have_their_own_registers(rp2040_factory):
    chip = rp2040_factory()
    chip.write_uint32(IO + PROC0_INTE0, LEVEL_LOW)  # GPIO0, proc 0
    chip.write_uint32(IO + PROC1_INTE0, EDGE_HIGH << 4)  # GPIO1, proc 1
    chip.write_uint32(IO + DORMANT_INTE0, LEVEL_HIGH << 8)  # GPIO2, dormant wake
    assert chip.read_uint32(IO + PROC0_INTE0) == LEVEL_LOW
    assert chip.read_uint32(IO + PROC1_INTE0) == EDGE_HIGH << 4
    assert chip.read_uint32(IO + DORMANT_INTE0) == LEVEL_HIGH << 8
    assert chip.read_uint32(IO + PROC0_INTS0) == LEVEL_LOW  # GPIO0 is low: (INTR & INTE) | INTF
    assert chip.read_uint32(IO + PROC1_INTS0) == 0 and chip.read_uint32(IO + DORMANT_INTS0) == 0
    chip.gpio[1].set_input_value(True)
    assert chip.read_uint32(IO + PROC1_INTS0) == EDGE_HIGH << 4
    assert chip.read_uint32(IO + PROC0_INTS0) == LEVEL_LOW  # proc 0 is not enabled for GPIO1
    chip.write_uint32(IO + PROC0_INTS0, 0xFFFFFFFF)  # read only
    assert chip.read_uint32(IO + PROC0_INTS0) == LEVEL_LOW


def test_io_qspi_has_its_own_layout(rp2040_factory):
    chip = rp2040_factory()
    chip.write_uint32(IO_QSPI + 0x34, 0x00F00000)  # PROC0_INTE: bits 23:20 are GPIO_QSPI_SD3's four
    assert chip.qspi[5].irq_enable_mask == 0xF and chip.read_uint32(IO_QSPI + 0x34) == 0x00F00000
    chip.write_uint32(IO_QSPI + 0x44, 0x1)  # PROC1_INTF
    assert chip.read_uint32(IO_QSPI + 0x44) == 0x1 and chip.qspi[0].irq_force_mask == 0
    assert chip.read_uint32(IO_QSPI + 0x30) & 0xF  # INTR: the SCLK pin's level bits


def test_a_qspi_pin_driven_by_sio_takes_gpio_hi_out_not_gpio_out(rp2040_factory):
    chip = rp2040_factory()
    chip.write_uint32(IO_QSPI + 0x0C, FUNCSEL_SIO)  # GPIO_QSPI_SS_CTRL (QSPI pin 1)
    chip.write_uint32(SIO + GPIO_HI_OE, 1 << 1)
    chip.write_uint32(SIO + GPIO_HI_OUT, 1 << 1)
    assert chip.qspi[1].value == GPIOPinState.HIGH
    chip.write_uint32(SIO + GPIO_HI_OUT, 0)
    assert chip.qspi[1].value == GPIOPinState.LOW
    chip.write_uint32(SIO + 0x20, 1 << 1)  # GPIO_OE bit 1 is GPIO1's: it must not enable the QSPI pin
    chip.write_uint32(SIO + GPIO_HI_OE, 0)
    chip.write_uint32(SIO + 0x10, 1 << 1)  # nor may GPIO_OUT bit 1 drive it
    assert chip.qspi[1].value != GPIOPinState.HIGH


PADS_QSPI = 0x40020000


def test_pads_qspi_lists_its_pads_in_its_own_order(rp2040_factory):
    chip = rp2040_factory()
    # PADS_QSPI: SCLK 0x04, SD0 0x08, SD1 0x0C, SD2 0x10, SD3 0x14, SS 0x18 - while RP2040.qspi (and IO_QSPI) are SCLK, SS, SD0..SD3
    assert chip.read_uint32(PADS_QSPI + 0x04) == 0x56  # SCLK: pull-down
    assert chip.read_uint32(PADS_QSPI + 0x18) == 0x5A  # SS: pull-up - what holds the flash deselected (record 0050)
    for offset in (0x08, 0x0C, 0x10, 0x14):
        assert chip.read_uint32(PADS_QSPI + offset) == 0x52  # SD0..SD3: no pull
    chip.write_uint32(PADS_QSPI + 0x08, 0x40 | 0x8)  # SD0: input enable and a pull-up
    assert chip.qspi[2].pad_value == 0x48 and chip.qspi[1].pad_value == 0x5A


def test_a_pad_with_output_disabled_is_not_driven_whatever_the_function_says(rp2040_factory):
    """PADS.OD "has priority over output enable from peripherals" (datasheet, GPIOn register): SIO drives the pin high, then OD is set and the pad lets go (to its pull)."""
    chip = rp2040_factory()
    chip.write_uint32(IO + 8 * 3 + 4, FUNCSEL_SIO)
    chip.write_uint32(SIO + 0x24, 1 << 3)  # GPIO_OE_SET
    chip.write_uint32(SIO + 0x14, 1 << 3)  # GPIO_OUT_SET
    assert chip.gpio[3].value == GPIOPinState.HIGH
    chip.write_uint32(PADS + 4 + 4 * 3, 0x56 | 0x80)  # OD
    assert chip.gpio[3].value == GPIOPinState.INPUT_PULL_DOWN
    chip.write_uint32(PADS + 4 + 4 * 3, 0x56)
    assert chip.gpio[3].value == GPIOPinState.HIGH


def test_a_qspi_pin_driven_by_sio_tells_its_listeners(rp2040_factory):
    """GPIO_HI_OUT / GPIO_HI_OE drive the QSPI pins (table 282): the listeners of a QSPI pin hear a change SIO makes, as they do for a GPIO pin."""
    chip = rp2040_factory()
    heard = []
    chip.qspi[1].add_listener(lambda new, old: heard.append(new))
    chip.write_uint32(0x40018000 + 8 * 1 + 4, FUNCSEL_SIO)
    heard.clear()
    chip.write_uint32(SIO + GPIO_HI_OE, 1 << 1)
    chip.write_uint32(SIO + GPIO_HI_OUT, 1 << 1)
    chip.write_uint32(SIO + GPIO_HI_OUT, 0)
    assert heard == [GPIOPinState.LOW, GPIOPinState.HIGH, GPIOPinState.LOW]


def test_a_pin_the_chip_drives_reads_back_its_own_level(rp2040_factory):
    """ "The input is always connected, so software can check the state of GPIOs at any time" (2.19.2): GPIO_IN shows the level SIO drives onto an output pin, and so does STATUS.INFROMPAD."""
    chip = rp2040_factory()
    chip.write_uint32(IO + 8 * 3 + 4, FUNCSEL_SIO)
    chip.write_uint32(SIO + 0x24, 1 << 3)  # GPIO_OE_SET
    assert (chip.read_uint32(SIO + 4) >> 3) & 1 == 0
    chip.write_uint32(SIO + 0x14, 1 << 3)  # GPIO_OUT_SET
    assert (chip.read_uint32(SIO + 4) >> 3) & 1 == 1
    assert chip.read_uint32(IO + 8 * 3) & (1 << 17)  # INFROMPAD
    chip.write_uint32(SIO + 0x18, 1 << 3)  # GPIO_OUT_CLR
    assert (chip.read_uint32(SIO + 4) >> 3) & 1 == 0
    chip.write_uint32(SIO + 0x18, 1 << 3)
    chip.write_uint32(SIO + 0x28, 1 << 3)  # GPIO_OE_CLR: released, the pull-down decides
    chip.write_uint32(SIO + 0x14, 1 << 3)
    assert (chip.read_uint32(SIO + 4) >> 3) & 1 == 0


def test_a_pin_the_chip_drives_raises_its_own_edge_interrupts(rp2040_factory):
    """An edge interrupt is a transition of the pad, whoever causes it: the chip toggling its own output latches EDGE_HIGH then EDGE_LOW."""
    chip = rp2040_factory()
    chip.write_uint32(IO + 8 * 3 + 4, FUNCSEL_SIO)
    chip.write_uint32(SIO + 0x24, 1 << 3)
    chip.write_uint32(SIO + 0x14, 1 << 3)
    assert (chip.read_uint32(IO + INTR0) >> 12) & EDGE_HIGH
    chip.write_uint32(IO + INTR0, 0xFFFFFFFF)
    assert (chip.read_uint32(IO + INTR0) >> 12) & (EDGE_HIGH | EDGE_LOW) == 0
    chip.write_uint32(SIO + 0x18, 1 << 3)
    assert (chip.read_uint32(IO + INTR0) >> 12) & EDGE_LOW


def test_an_external_drive_wins_over_the_chips_own_output(rp2040_factory):
    """No drive strengths are modelled to settle a contention: what is wired to the pin (a button, a device) wins over the chip's output, and releasing it hands the pad back to the chip."""
    chip = rp2040_factory()
    chip.write_uint32(IO + 8 * 3 + 4, FUNCSEL_SIO)
    chip.write_uint32(SIO + 0x24, 1 << 3)
    chip.write_uint32(SIO + 0x14, 1 << 3)
    chip.gpio[3].set_input_value(False)
    assert (chip.read_uint32(SIO + 4) >> 3) & 1 == 0
    chip.gpio[3].release_input()
    assert (chip.read_uint32(SIO + 4) >> 3) & 1 == 1


def test_a_qspi_pin_not_under_sio_is_not_read_as_driven_low(rp2040_factory):
    """The XIP side that drives the QSPI pins is not modelled: an undriven QSPI_SS still reads its pull-up (the BOOTSEL read and CircuitPython's poll rely on it)."""
    chip = rp2040_factory()
    assert (chip.read_uint32(SIO + 8) >> 1) & 1 == 1  # GPIO_HI_IN: SS high through its pull-up
