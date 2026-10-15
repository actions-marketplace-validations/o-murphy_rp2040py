"""The DMA example use cases of the datasheet (2.5.6), run as the datasheet writes them - the same register values the pico-examples code (`dma/control_blocks`, `dma/channel_irq`) produces
through the SDK's helpers - against the real chip (both builds), with the UART as the paced peripheral.

Record 0098 ("DMA second pass: the example chapters"). What these check that the register-level tests do not is the *interplay*: a control channel that reprograms a data channel through an
alias, a write ring wrapping on a register pair, CHAIN_TO between the two, an IRQ_QUIET channel that raises its interrupt only on the null trigger that ends the list, and a channel
re-triggered from its own completion interrupt.
"""

from rp2040py.irq import IRQ
from rp2040py.peripherals.dma import DREQChannel
from rp2040py.rp2040 import RP2040

DMA = 0x50000000
UART0 = 0x40034000
INTR, INTE0, INTS0 = DMA + 0x400, DMA + 0x404, DMA + 0x40C
MULTI_CHAN_TRIGGER = DMA + 0x430

# CTRL fields (pico-sdk hardware_structs/dma.h): EN 0, HIGH_PRIORITY 1, DATA_SIZE 3:2, INCR_READ 4, INCR_WRITE 5, RING_SIZE 9:6, RING_SEL 10, CHAIN_TO 14:11, TREQ_SEL 20:15, IRQ_QUIET 21
EN, INCR_READ, INCR_WRITE, RING_SEL, IRQ_QUIET = 1, 1 << 4, 1 << 5, 1 << 10, 1 << 21
SIZE_8, SIZE_32 = 0 << 2, 2 << 2
TREQ_PERMANENT = 0x3F


def _ctrl(*, size, read_inc, write_inc, dreq, chain_to, ring_bits=0, ring_write=False, quiet=True):
    """`dma_channel_get_default_config` plus the `channel_config_set_*` calls of the examples."""
    value = EN | size | (INCR_READ if read_inc else 0) | (INCR_WRITE if write_inc else 0)
    value |= (
        (ring_bits << 6)
        | (RING_SEL if ring_write else 0)
        | (chain_to << 11)
        | (dreq << 15)
        | (IRQ_QUIET if quiet else 0)
    )
    return value


def _store(chip, address, data: bytes):
    for i, byte in enumerate(data):
        chip.write_uint8(address + i, byte)


def _uart_at_full_speed(chip):
    """pico-sdk `uart_init`: a baud rate (here the fastest, divisor 1), FIFOs on, UART/TX/RX enabled, both DMA enables."""
    sent = []
    chip.uart[0].on_byte = sent.append
    chip.write_uint32(UART0 + 0x24, 1)  # IBRD
    chip.write_uint32(UART0 + 0x2C, 0x70)  # LCR_H: 8 bits, FEN
    chip.write_uint32(UART0 + 0x30, 0x301)  # CR
    chip.write_uint32(UART0 + 0x48, 0x3)  # DMACR: TXDMAE | RXDMAE
    return sent


def test_the_control_blocks_example_gathers_six_buffers_into_the_uart():
    """dma/control_blocks: a control channel loads {length, address} pairs into the data channel's alias 3 (TRANS_COUNT, then the READ_ADDR trigger) through a two-word write ring, the data
    channel chains back to it, and a null block ends the list with the data channel's IRQ."""
    chip = RP2040()
    sent = _uart_at_full_speed(chip)
    words = [b"Transferring ", b"one ", b"word ", b"at ", b"a ", b"time.\n"]
    base = 0x20010000
    blocks = base  # the control_blocks[] array: seven {len, data} pairs, the last {0, NULL}
    text = base + 0x100
    cursor = text
    for index, word in enumerate(words):
        _store(chip, cursor, word)
        chip.write_uint32(blocks + 8 * index, len(word))
        chip.write_uint32(blocks + 8 * index + 4, cursor)
        cursor += len(word)
    chip.write_uint32(blocks + 8 * len(words), 0)
    chip.write_uint32(blocks + 8 * len(words) + 4, 0)

    ctrl_chan, data_chan = 0, 1
    ctrl = DMA + ctrl_chan * 0x40
    data = DMA + data_chan * 0x40
    # the control channel: 32-bit, both addresses increment, the write pointer wraps on 1 << 3 bytes, count 2, not started
    chip.write_uint32(ctrl + 0x00, blocks)  # READ_ADDR
    chip.write_uint32(ctrl + 0x04, data + 0x38)  # WRITE_ADDR = &ch[data].al3_transfer_count
    chip.write_uint32(ctrl + 0x08, 2)  # TRANS_COUNT
    chip.write_uint32(
        ctrl + 0x10,  # AL1_CTRL: the CTRL write that does not trigger
        _ctrl(
            size=SIZE_32,
            read_inc=True,
            write_inc=True,
            dreq=TREQ_PERMANENT,
            chain_to=ctrl_chan,
            ring_bits=3,
            ring_write=True,
        ),
    )
    # the data channel: 8-bit, read increments, UART TX paced, chained to the control channel, quiet
    chip.write_uint32(data + 0x04, UART0 + 0x00)  # WRITE_ADDR = &uart->dr
    chip.write_uint32(data + 0x08, 0)
    chip.write_uint32(
        data + 0x10,
        _ctrl(
            size=SIZE_8,
            read_inc=True,
            write_inc=False,
            dreq=int(DREQChannel.DREQ_UART0_TX),
            chain_to=ctrl_chan,
            quiet=True,
        ),
    )
    chip.write_uint32(MULTI_CHAN_TRIGGER, 1 << ctrl_chan)  # dma_start_channel_mask(1u << ctrl_chan)

    for _ in range(400):
        chip.clock.tick(1000)
        if chip.read_uint32(INTR) & (1 << data_chan):
            break
    # the whole message fits the UART's 32-entry TX FIFO plus the shift register, so the DMA finishes the list at once; the bytes leave at the baud rate
    chip.clock.tick(100_000)
    assert bytes(sent) == b"Transferring one word at a time.\n"
    assert (
        chip.read_uint32(INTR) == 1 << data_chan
    )  # only the data channel, and only for the null trigger at the end of the list ("irq_quiet")
    chip.write_uint32(DMA + 0x40C, 1 << data_chan)  # dma_hw->ints0 = 1u << data_chan clears it
    assert chip.read_uint32(INTR) == 0


def test_the_channel_irq_example_retriggers_a_paced_channel_from_its_completion_interrupt():
    """dma/channel_irq: one channel writes the same word again and again to a paced peripheral (read address fixed), raises IRQ 0 at the end of each block, and the handler gives it a new read
    address and triggers it again. (The example paces with a PIO TX FIFO; the UART's TX FIFO stands in for it - the same DREQ mechanism.)"""
    chip = RP2040()
    sent = _uart_at_full_speed(chip)
    wavetable = 0x20020000
    for level in range(4):
        chip.write_uint32(wavetable + 4 * level, 0x41 + level)
    chan = 2
    ch = DMA + chan * 0x40
    repeat = 3
    # dma_channel_configure(chan, &c, &uart->dr, NULL, 3, false); the channel interrupt on IRQ 0
    chip.write_uint32(ch + 0x04, UART0)
    chip.write_uint32(ch + 0x08, repeat)
    chip.write_uint32(
        ch + 0x10,
        _ctrl(
            size=SIZE_8,
            read_inc=False,
            write_inc=False,
            dreq=int(DREQChannel.DREQ_UART0_TX),
            chain_to=chan,
            quiet=False,
        ),
    )
    chip.write_uint32(INTE0, 1 << chan)
    nvic_pending = []
    original = chip.set_interrupt
    chip.set_interrupt = lambda irq, level: (nvic_pending.append((int(irq), bool(level))), original(irq, level))[1]  # type: ignore[method-assign]

    for level in range(4):
        # the handler: ints0 = 1 << chan ; dma_channel_set_read_addr(chan, &wavetable[level], true)  (the READ_ADDR_TRIG alias 0x3C)
        chip.write_uint32(INTS0, 1 << chan)
        chip.write_uint32(ch + 0x3C, wavetable + 4 * level)
        for _ in range(100):
            chip.clock.tick(1000)
            if chip.read_uint32(INTR) & (1 << chan):
                break
        assert chip.read_uint32(INTS0) & (1 << chan), f"block {level} did not raise its interrupt"
        assert (int(IRQ.DMA_IRQ0), True) in nvic_pending
    chip.clock.tick(20_000)
    assert bytes(sent) == b"AAABBBCCCDDD"  # three of each word, in order, one block per interrupt
