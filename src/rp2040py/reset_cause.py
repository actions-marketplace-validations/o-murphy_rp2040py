"""Why a chip-level reset happened - the one definition the chip and the device layer share (docs/records/0096-cpp-mcu-core.md, step 1, F1)."""

from enum import Enum, auto

__all__ = ("ResetCause",)


class ResetCause(Enum):
    """Why a chip-level reset happened - what `RP2040.enter_reset()` records so the firmware coming back up
    can report it (`machine.reset_cause()` on MicroPython, `microcontroller.cpu.reset_reason` on
    CircuitPython).

    The three are genuinely different signatures in hardware, not labels on one event
    (docs/records/0089-one-reset-for-every-trigger.md §1.3):

    - `WATCHDOG` - the guest reset itself, via the watchdog's TRIGGER bit (`machine.reset()`,
      `microcontroller.reset()`, `mpremote reset`) or a watchdog timeout. The watchdog block keeps
      its own REASON/SCRATCH bookkeeping across it, and CHIP_RESET is left alone; both firmwares
      read that as WDT/SOFTWARE/WATCHDOG.
    - `RUN_PIN` - the RESET button pulling RUN low, and the default for a host-side reset (0089's
      D4: what "reset the board" corresponds to physically). Clears the watchdog's REASON/scratch
      and sets CHIP_RESET.HAD_RUN; firmware reads PWRON/RESET_PIN.
    - `POWER_ON` - a power cycle/brown-out. Same clearing, but CHIP_RESET.HAD_POR - which is also
      the state a freshly constructed chip already starts in.

    The chip records the cause itself (`RP2040.enter_reset(cause=...)`), after the reset: a chip with no device layer
    around it (`rp2040py run`, a board file) reports the same cause a device does.
    """

    POWER_ON = auto()
    RUN_PIN = auto()
    WATCHDOG = auto()
