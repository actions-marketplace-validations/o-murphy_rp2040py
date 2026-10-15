"""The pure-Python reference of the pin bank: the object a chip's pins are slots of (docs/records/0096-cpp-mcu-core.md, step 0 (b)).

The native build keeps all the pins of a bank in one C++ `PinBank` (`native/_gpio_pin.pyx`'s `GPIOPinBank`); a chip asks the *bank* for what is a property of
all the pins at once - `input_values()` (SIO's GPIO_IN) and `any_irq_value()` (the level of the IO_BANK0 interrupt line) - instead of looping over its pins.
This class gives the pure-Python chip the same object with the same interface, so that the two chip classes do not differ in how they ask; here the answer is
a loop over the pins' own properties, which is the reference the C++ bank is held to (`tests/utils/pin_diff.py`).

`shells` is the list of the pin objects, one slot per pin (`None` until its pin is built); a chip's `gpio` / `qspi` list is this very list.
"""

from typing import Any

__all__ = ("GPIOPinBank",)


class GPIOPinBank:
    def __init__(self, rp2040: Any, count: int, always_output_enabled: bool = False, first: int = 0) -> None:
        if count < 1 or count > 32:
            raise ValueError(f"a pin bank holds 1 to 32 pins, not {count}")
        self.rp2040 = rp2040
        self.first = first
        self.always_output_enabled = bool(always_output_enabled)
        self.shells: list[Any] = [None] * count

    def __len__(self) -> int:
        return len(self.shells)

    def input_values(self) -> int:
        """One bit per pin: what SIO's GPIO_IN returns."""
        result = 0
        for index, pin in enumerate(self.shells):
            if pin.input_value:
                result |= 1 << index
        return result

    def any_irq_value(self) -> bool:
        """Is any pin's IRQ value set: the level of the IO interrupt line."""
        for pin in self.shells:
            if pin.irq_value:
                return True
        return False
