"""Pure-Python reference implementation of `SimulationClock`/`ClockAlarm`. `simulation_clock.py`
is the public facade that picks this or the native `rp2040py.native._simulation_clock` port;
callers should import `SimulationClock`/`ClockAlarm` from there, never from here directly - same
relationship `_state_machine.py` has to `state_machine.py`.
"""

from __future__ import annotations

import math
from collections.abc import Callable

from rp2040py.clock.clock import AlarmCallback, IAlarm, IClock

__all__ = (
    "MAX_TICKS",
    "TICKS_PER_NANO",
    "ClockAlarm",
    "SimulationClock",
    "nanos_to_ticks",
)

# Time is an INTEGER count of ticks of 1/16 ns (docs/records/0100-integer-clock.md): 18 years in an int64 (the C++ clock), exactly - the `float` nanoseconds this replaced stopped resolving 8 ns
# beyond 104 days. A 125 MHz cycle is 128 ticks.
TICKS_PER_NANO = 16
MAX_TICKS = (1 << 63) // 2 - 1  # "never": times saturate here instead of growing past the C++ clock's int64


def nanos_to_ticks(nanos: float) -> int:
    """A delta in nanoseconds as ticks: round half up, saturating at +-MAX_TICKS, NaN as 0 - the same function as `nanos_to_ticks` in core/clock.hpp."""
    if math.isnan(nanos):
        return 0
    ticks = nanos * TICKS_PER_NANO
    if ticks >= MAX_TICKS:
        return MAX_TICKS
    if ticks <= -MAX_TICKS:
        return -MAX_TICKS
    return math.floor(ticks + 0.5)


ClockEventCallback = Callable[[], None]


class ClockAlarm(IAlarm):
    def __init__(self, clock: SimulationClock, callback: AlarmCallback):
        self._clock = clock
        self.callback = callback
        self.next: ClockAlarm | None = None
        self.ticks: int = 0  # the absolute due time while linked
        self.scheduled = False

    @property
    def nanos(self) -> float:
        return self.ticks / TICKS_PER_NANO

    def schedule(self, delta_nanos: float) -> None:
        if self.scheduled:
            self.cancel()
        self._clock.link_alarm(delta_nanos, self)

    def schedule_ticks(self, delta_ticks: int) -> None:
        if self.scheduled:
            self.cancel()
        self._clock.link_alarm_ticks(delta_ticks, self)

    def cancel(self) -> None:
        self._clock.unlink_alarm(self)
        self.scheduled = False


class SimulationClock(IClock):
    def __init__(self, frequency: float = 125e6):
        self.frequency = frequency
        self._next_alarm: ClockAlarm | None = None
        self._ticks: int = 0
        self._cycle_remainder = 0.0
        self.saturated = False  # a time was clamped to MAX_TICKS

    @property
    def ticks(self) -> int:
        """The time in ticks of 1/16 ns, exactly."""
        return self._ticks

    @property
    def nanos(self) -> float:
        """The time in nanoseconds, derived (inexact beyond 2**53 / 16 ns: take differences in `ticks`)."""
        return self._ticks / TICKS_PER_NANO

    def _to_ticks(self, nanos: float) -> int:
        """`nanos_to_ticks`, recording a clamp."""
        ticks = nanos_to_ticks(nanos)
        if ticks in (MAX_TICKS, -MAX_TICKS):
            self.saturated = True
        return ticks

    def _add_ticks(self, a: int, b: int) -> int:
        if b > 0 and a > MAX_TICKS - b:
            self.saturated = True
            return MAX_TICKS
        return a + b

    @property
    def micros(self) -> float:
        return self.nanos / 1000

    def create_alarm(self, callback: ClockEventCallback) -> ClockAlarm:
        return ClockAlarm(self, callback)

    def link_alarm(self, nanos: float, alarm: ClockAlarm) -> ClockAlarm:
        return self.link_alarm_ticks(self._to_ticks(nanos), alarm)

    def link_alarm_ticks(self, ticks: int, alarm: ClockAlarm) -> ClockAlarm:
        alarm.ticks = self._add_ticks(self._ticks, ticks)
        alarm_list_item = self._next_alarm
        last_item = None
        # `<=`, not `<`: a same-timestamp alarm already in the list must fire before one just
        # inserted "at" that same instant (FIFO tie-break), or a self-rescheduling zero-delay
        # producer (e.g. a DMA TX channel) can perpetually cut in front of an already-pending
        # zero-delay consumer alarm (e.g. that same transfer's paired RX channel) and starve it -
        # see docs/tasks/main-spi-hang.md.
        while alarm_list_item and alarm_list_item.ticks <= alarm.ticks:
            last_item = alarm_list_item
            alarm_list_item = alarm_list_item.next
        if last_item:
            last_item.next = alarm
            alarm.next = alarm_list_item
        else:
            self._next_alarm = alarm
            alarm.next = alarm_list_item
        alarm.scheduled = True
        return alarm

    def unlink_alarm(self, alarm: ClockAlarm) -> bool:
        alarm_list_item = self._next_alarm
        if not alarm_list_item:
            return False
        last_item = None
        while alarm_list_item:
            if alarm_list_item is alarm:
                if last_item:
                    last_item.next = alarm_list_item.next
                else:
                    self._next_alarm = alarm_list_item.next
                return True
            last_item = alarm_list_item
            alarm_list_item = alarm_list_item.next
        return False

    def tick(self, delta_nanos: float) -> None:
        self.tick_ticks(self._to_ticks(delta_nanos))

    def tick_ticks(self, delta_ticks: int) -> None:
        target = self._add_ticks(self._ticks, delta_ticks)
        alarm = self._next_alarm
        while alarm and alarm.ticks <= target:
            self._next_alarm = alarm.next
            self._ticks = alarm.ticks
            alarm.callback()
            alarm = self._next_alarm
        self._ticks = target

    def advance_cycles(self, cycles: int, ticks_per_cycle: float) -> None:
        """Advances by `cycles` periods of `ticks_per_cycle` ticks each (not a whole number in general: 120.30 at 133 MHz) with the fraction CARRIED between calls, so the long-run rate is exact
        however the cycles are split into calls - the CPU's time, one call per instruction."""
        self._cycle_remainder += cycles * ticks_per_cycle
        whole = math.floor(self._cycle_remainder)
        self._cycle_remainder -= whole
        self.tick_ticks(min(MAX_TICKS, whole))

    @property
    def ticks_to_next_alarm(self) -> int:
        if self._next_alarm:
            return self._next_alarm.ticks - self._ticks
        return 0

    @property
    def nanos_to_next_alarm(self) -> float:
        return self.ticks_to_next_alarm / TICKS_PER_NANO

    @property
    def has_scheduled_alarm(self) -> bool:
        """Distinguishes "no alarm scheduled at all" from "an alarm is scheduled and due right
        now" - both read as `nanos_to_next_alarm == 0`, which is fine for that property's own two
        existing callers (both just want "how long to jump forward," and jumping forward by 0 when
        nothing's scheduled is already correct) but is genuinely ambiguous for
        simulator.py's opt-in clock-tick-batching (RP2040PY_CLOCK_TICK_BATCH): with no alarm
        scheduled, there's no due time to avoid overshooting, so batching can run unbounded rather
        than conservatively flushing every instruction."""
        return self._next_alarm is not None
