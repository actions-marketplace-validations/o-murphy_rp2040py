# USB controller: audit against the datasheet, then the C++ port (working note)

Status: in progress (2026-10-13). Recipe: [core-block-recipe.md](../reference/core-block-recipe.md), section A first. Folded into a record (0101) when done.

Sources read: RP2040 datasheet 4.1 (text of `pdftotext -layout`), pico-sdk `hardware_regs/usb.h`, TinyUSB `src/portable/raspberrypi/rp2040/{dcd,hcd}_rp2040.c`, `rp2040_usb.c/.h`, pico-sdk `rp2040_usb_device_enumeration.c`.
`python scripts/audit/datasheet_conformance.py rp2040.txt USB` (USB added to its `BASES`): 23 registers, 11 with findings.

## What the model is

`peripherals/usb.py` (`RPUSBController`) is the controller's register file plus the DPRAM (4 kB, in the bus memory map: a completed 32-bit write to it reaches `dpram_updated()`), with the *other end of the wire* abstracted away:
- **Device mode** (the firmware is the device; MicroPython, CircuitPython, Kaluma, pico-sdk CDC): the other end is `usb/cdc.py` (`USBCDC`), a host *shortcut* - it injects setup packets (`send_setup_packet`), receives IN data (`on_endpoint_write`) and supplies OUT data (`endpoint_read_done`). There are no token/handshake packets: a buffer is delivered the moment the firmware makes it AVAILABLE.
- **Host mode** (the firmware is a TinyUSB host): the other end is a Python `USBDevice` (`usb/usb_device.py`), called with setup/in/out and answering ack/nak/stall.
The port keeps this split: the block (registers, DPRAM semantics, interrupts, SOF, alarms) in C++; the host shortcut and the `USBDevice` objects stay Python, reached through host functions. A token-level wire is a different (larger) project and is NOT part of this port.

## Findings (datasheet / SDK vs. the reference), by decision

Fix in the reference first (clear defects; each gets a test citing its source):
1. `USB_PWR` bit layout is wrong. Datasheet table 414 / `usb.h`: VBUS_EN 0, VBUS_EN_OVERRIDE_EN 1, VBUS_DETECT 2, VBUS_DETECT_OVERRIDE_EN 3, OVERCURR_DETECT 4, OVERCURR_DETECT_EN 5; the reference has VBUS_DETECT = 3 and OVERRIDE_EN = 2 (it only works because firmware sets both). `SIE_STATUS.VBUS_OVER_CURR` (RO) is never driven from OVERCURR_DETECT(+EN).
2. `USB_MUXING` is not stored (reads 0; a write only has the enumeration-fix side effect); datasheet: RW, 4 bits.
3. `USBPHY_DIRECT`, `USBPHY_DIRECT_OVERRIDE`, `USBPHY_TRIM` (0x7C/0x80/0x84) are absent (read warns). TRIM resets to 0x1F1F, the others are RW; the RX_* / OVV / OVCN bits of PHY_DIRECT are RO (no analogue model: they read 0 - "not independently sourced"). The enumeration fix of the SDK writes `phy_direct`/`phy_direct_override`.
4. `NAK_POLL` resets to 0x00100010 (DELAY_FS 25:16, DELAY_LS 9:0); the reference resets to 0 and keeps all 32 bits.
5. Write masks: `ADDR_ENDPn` (26, 25, 19:16, 6:0), `INT_EP_CTRL` (15:1), `SIE_CTRL` (reserved bits; RESET_BUS, RESUME, STOP_TRANS, START_TRANS are self-clearing), `EP_STALL_ARM` (1:0), `USB_PWR` (5:0), `NAK_POLL`, `ADDR_ENDP` (19:16, 6:0 - host and device share one mask).
6. `SIE_STATUS` write-1-to-clear misses `SPEED` (9:8) and `SUSPENDED` (4) (both WC); `SOF_WR` is write-only ("WF").
7. `BUFF_CPU_SHOULD_HANDLE` always reads 0; it must say which buffer (0/1) of a double-buffered endpoint completed (TinyUSB's current `handle_hw_buff_status` reads it).
8. `INTR` is derived per mode from a table, and misses bits the datasheet defines whatever the mode: `TRANS_COMPLETE` in device mode, `EP_STALL_NAK` (from `EP_STATUS_STALL_NAK`), `ABORT_DONE` (from `EP_ABORT_DONE`), `DEV_SOF`/`HOST_SOF` (cleared by reading `SOF_RD`; the reference never clears them), `DEV_CONN_DIS`/`HOST_CONN_DIS` clear rules.
9. Device-mode buffer status write-back: after a buffer completes the hardware writes its half of the buffer-control register as length | pid | last-buffer with everything else zero (FULL set only for OUT); the reference clears only AVAILABLE/FULL and leaves STALL, RESET_SEL, etc. in place. `LAST_BUFFER` on a completed IN/OUT must raise `SIE_STATUS.TRANS_COMPLETE` (device mode: never raised now).
10. `BUFF_STATUS` is raised only when the endpoint's interrupt enable allows it (EP ctrl bit 29 every buffer / bit 28 every 2 buffers, double buffered; for EP0 `SIE_CTRL.EP0_INT_1BUF/2BUF`); the reference raises it for a single-buffered IN regardless, and at the time the buffer is *queued* rather than when its status phase completes.
11. A setup packet clears `EP_STALL_ARM` (4.1.2.8.1).
12. IN needs AVAILABLE *and* FULL (4.1.2.8.2); the reference ignores FULL.

Document, do not implement (each needs its own go-ahead; they need a token-level host or a time base the shortcut lacks):
- STALL / NAK responses and `EP_STATUS_STALL_NAK` (IRQ_ON_STALL / IRQ_ON_NAK), `EP_STALL_ARM` gating of an EP0 STALL, `EP_ABORT` (the reference marks `EP_ABORT_DONE` at once).
- Device SOF (`DEV_SOF`, `SOF_RD` from the host), suspend (no SOF for 3 ms) and resume (`SIE_CTRL.RESUME`), `DEV_CONN_DIS` from VBUS, `SIE_STATUS.LINE_STATE` beyond J/SE0.
- Host: erratum RP2040-E4 (status written to the wrong half of the buffer-control register), NAK retry timing (`NAK_POLL` is stored only), low speed / preamble, the `INT_EP` polling interval model.
- The B0/B1 enumeration fix (`rp2040_chip_version() == 1`): the emulated chip is not B1, the fix is not run; `USB_MUXING` with `TO_DIGITAL_PAD` keeps the `CONNECTED` shortcut the reference has.

Not independently sourced: the 10 us / 5 us transfer delays (`read_delay_microseconds` etc. - "determined empirically" in the reference).

## Plan

1. This note + tracker row; the reference fixes (1-12 above) with `tests/test_usb_datasheet.py`, in their own commit.
2. `peripherals/usb.py` -> `_usb.py` + facade; oracle `tests/utils/usb_diff.py` (two chips, the same operation stream incl. a scripted host shortcut), with source-text mutants.
3. `native/core/usb.hpp` + `tests/cpp/test_usb.cpp` + `native/_usb.pyx/.pxd/.pyi`; the DPRAM bytes are the bus region's.
4. Live boots (MicroPython, CircuitPython, Kaluma, pico-sdk, a TinyUSB host if there is one), docs, register coverage, tracker.
