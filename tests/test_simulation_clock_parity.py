"""The native clock (C++ `Clock`, docs/records/0096-cpp-mcu-core.md Phase 2) against the pure-Python reference.

Randomized scripts of alarm operations - schedule, reschedule, cancel, tick - with callbacks that themselves
re-arm, arm or cancel other alarms, or raise, are run on `clock/_simulation_clock.py` and on
`native/_simulation_clock.pyx`. The firing log, every observable (`nanos`, `nanos_to_next_alarm`,
`has_scheduled_alarm`) after every operation, and the exceptions that escape `tick()` must be identical.
Then the two properties that changed with the port rather than with the arithmetic: a scheduled alarm that
nothing else references still fires, and a dropped clock with alarms pending is garbage-collected.
"""

import gc
import random
import weakref

import pytest

pytest.importorskip("rp2040py.native._simulation_clock", reason="the native extension is not built")

from rp2040py.clock._simulation_clock import SimulationClock as PureClock
from rp2040py.native._simulation_clock import SimulationClock as NativeClock


class Boom(Exception):
    pass


class World:
    """One clock with N alarms whose callbacks follow a fixed per-alarm program."""

    def __init__(self, clock_cls, programs):
        self.clock = clock_cls()
        self.log = []
        self.programs = programs
        self.counters = [dict.fromkeys(("resched", "arm", "cancel", "boom"), 0) for _ in programs]
        self.alarms = [self.clock.create_alarm(self._callback(i)) for i in range(len(programs))]

    def _callback(self, i):
        def fire():
            self.log.append(("fire", i, self.clock.nanos))
            program, count = self.programs[i], self.counters[i]
            if program["resched"] and count["resched"] < program["resched"][1]:
                count["resched"] += 1
                self.alarms[i].schedule(program["resched"][0])
            # Every action is bounded per alarm: two alarms that arm each other with zero delay would otherwise
            # loop inside a single tick() forever, in the reference as much as in the port.
            if program["arm"] and count["arm"] < 3:
                count["arm"] += 1
                other, delay = program["arm"]
                self.alarms[other].schedule(delay)
            if program["cancel"] is not None and count["cancel"] < 3:
                count["cancel"] += 1
                self.alarms[program["cancel"]].cancel()
            if program["boom"] and count["boom"] == 0:
                count["boom"] += 1
                raise Boom(i)

        return fire

    def observe(self):
        c = self.clock
        return (c.nanos, c.nanos_to_next_alarm, c.has_scheduled_alarm, c.ticks, c.ticks_to_next_alarm)

    def apply(self, op):
        kind = op[0]
        if kind == "schedule":
            self.alarms[op[1]].schedule(op[2])
        elif kind == "cancel":
            self.alarms[op[1]].cancel()
        elif kind == "schedule_ticks":
            self.alarms[op[1]].schedule_ticks(op[2])
        elif kind == "advance_cycles":
            try:
                self.clock.advance_cycles(op[1], op[2])
            except Boom as error:
                self.log.append(("raised", error.args[0]))
        elif kind == "tick_ticks":
            try:
                self.clock.tick_ticks(op[1])
            except Boom as error:
                self.log.append(("raised", error.args[0]))
        else:  # tick
            try:
                self.clock.tick(op[1])
            except Boom as error:
                self.log.append(("raised", error.args[0]))


def _delay(rng):
    return rng.choice([0, 0, 1, 2, 5, 10, 0.5, 3.25, rng.randrange(0, 200), rng.random() * 50])


def _script(seed):
    rng = random.Random(seed)
    n = rng.randrange(3, 9)
    programs = [
        {
            "resched": (_delay(rng), rng.randrange(1, 4)) if rng.random() < 0.3 else None,
            "arm": (rng.randrange(n), _delay(rng)) if rng.random() < 0.25 else None,
            "cancel": rng.randrange(n) if rng.random() < 0.2 else None,
            "boom": rng.random() < 0.15,
        }
        for _ in range(n)
    ]
    ops = []
    for _ in range(250):
        roll = rng.random()
        if roll < 0.35:
            ops.append(("schedule", rng.randrange(n), _delay(rng)))
        elif roll < 0.4:
            ops.append(("schedule_ticks", rng.randrange(n), rng.choice([0, 1, 7, 128, rng.randrange(0, 4000)])))
        elif roll < 0.45:
            ops.append(("tick_ticks", rng.choice([0, 1, 5, 128, rng.randrange(0, 3000)])))
        elif roll < 0.5:
            # a clock that is not a whole number of ticks per cycle: 133 MHz is 120.30 ticks
            ops.append(
                (
                    "advance_cycles",
                    rng.randrange(0, 12),
                    rng.choice([128.0, 120.30075187969925, 333.3333333333333, 41.666666666666664]),
                )
            )
        elif roll < 0.55:
            ops.append(("cancel", rng.randrange(n)))
        else:
            ops.append(("tick", _delay(rng) * rng.choice([1, 1, 3, 10])))
    return programs, ops


@pytest.mark.parametrize("seed", range(40))
def test_the_native_clock_matches_the_pure_python_clock(seed):
    programs, ops = _script(seed)
    pure, native = World(PureClock, programs), World(NativeClock, programs)

    for index, op in enumerate(ops):
        pure.apply(op)
        native.apply(op)
        assert native.observe() == pure.observe(), f"after op {index} {op}"
        assert native.log == pure.log, f"after op {index} {op}"

    # Drain whatever is still pending, so every alarm that was armed has to have fired identically.
    for _ in range(200):
        if not pure.clock.has_scheduled_alarm:
            break
        step = ("tick", pure.clock.nanos_to_next_alarm + 1)
        pure.apply(step)
        native.apply(step)
    assert native.observe() == pure.observe()
    assert native.log == pure.log
    assert any(entry[0] == "fire" for entry in pure.log), "the script should have fired something"


@pytest.mark.parametrize("clock_cls", [PureClock, NativeClock], ids=["pure", "native"])
def test_a_callback_that_raises_leaves_the_clock_at_that_alarm_s_time(clock_cls):
    clock = clock_cls()

    def fail():
        raise Boom("x")

    clock.create_alarm(fail).schedule(30)
    later = []
    clock.create_alarm(lambda: later.append(clock.nanos)).schedule(60)

    with pytest.raises(Boom):
        clock.tick(100)

    assert clock.nanos == 30  # not 100: the rest of the step was not applied
    assert clock.has_scheduled_alarm and clock.nanos_to_next_alarm == 30
    clock.tick(100)  # the pending alarm is still there and fires normally afterwards
    assert later == [60] and clock.nanos == 130


def test_the_native_clock_keeps_the_properties_the_hot_loop_reads():
    clock = NativeClock(2e6)

    assert clock.frequency == 2e6
    clock.frequency = 3e6
    assert clock.frequency == 3e6
    assert (clock.nanos, clock.micros, clock.nanos_to_next_alarm, clock.has_scheduled_alarm) == (0, 0, 0, False)
    clock.create_alarm(lambda: None).schedule(2500)
    assert clock.nanos_to_next_alarm == 2500 and clock.has_scheduled_alarm
    clock.tick(1000)
    assert (clock.nanos, clock.micros, clock.nanos_to_next_alarm) == (1000, 1, 1500)


@pytest.mark.parametrize("clock_cls", [PureClock, NativeClock], ids=["pure", "native"])
def test_a_scheduled_alarm_nothing_else_references_still_fires(clock_cls):
    """`clock.create_alarm(cb).schedule(n)` and drop it: the clock's own list used to be what kept the alarm
    alive. With raw pointers in C++, the native clock has to do that deliberately."""
    clock = clock_cls()
    fired = []
    clock.create_alarm(lambda: fired.append(clock.nanos)).schedule(10)
    gc.collect()

    clock.tick(20)

    assert fired == [10]


@pytest.mark.parametrize("clock_cls", [PureClock, NativeClock], ids=["pure", "native"])
def test_a_dropped_clock_with_pending_alarms_is_garbage_collected(clock_cls):
    """The clock <-> alarm cycle has to stay visible to the garbage collector: a chip that is dropped while
    alarms are pending must not leak (each one owns 16 MiB of flash)."""

    class Tracked(clock_cls):
        pass

    clock = Tracked()
    alarms = [clock.create_alarm(lambda: None) for _ in range(5)]
    for alarm in alarms:
        alarm.schedule(100)
    ref = weakref.ref(clock)

    del clock, alarm, alarms
    gc.collect()

    assert ref() is None


@pytest.mark.parametrize("clock_cls", [PureClock, NativeClock], ids=["pure", "native"])
def test_an_alarm_can_be_rescheduled_from_its_own_callback_many_times(clock_cls):
    clock = clock_cls()
    fired = []

    def fire():
        fired.append(clock.nanos)
        if len(fired) < 50:
            alarm.schedule(7)

    alarm = clock.create_alarm(fire)
    alarm.schedule(7)

    clock.tick(10_000)

    assert fired == [7.0 * (i + 1) for i in range(50)]
    assert not clock.has_scheduled_alarm


@pytest.mark.parametrize("clock_cls", [PureClock, NativeClock], ids=["pure", "native"])
def test_an_8_ns_step_still_resolves_after_more_than_2_to_the_53_ns(clock_cls):
    """The failure that started docs/records/0100: a `double` of nanoseconds past 2**53 (104 days) cannot hold `now + 8`, and a DMA's one-word-per-cycle schedule collapsed to one instant."""
    clock = clock_cls()
    clock.tick_ticks(2**53 * 16 + 7)  # ~104 days, then a bit
    fired = []
    for index in range(3):
        clock.create_alarm(lambda index=index: fired.append((index, clock.ticks))).schedule(8)
    start = clock.ticks
    clock.tick(8)
    assert fired == [(0, start + 128), (1, start + 128), (2, start + 128)]
    assert clock.ticks == start + 128
    clock.tick_ticks(128 * 1000)
    assert clock.ticks == start + 128 * 1001  # exact, a tick at a time, years in


@pytest.mark.parametrize("clock_cls", [PureClock, NativeClock], ids=["pure", "native"])
def test_the_year_2_5_of_the_failing_official_sequence_is_well_inside_the_range(clock_cls):
    clock = clock_cls()
    clock.tick_ticks(int(7.8e16 * 16))  # the reading `clock.nanos` had when rp2_dma failed
    ticks = clock.ticks
    clock.create_alarm(lambda: None).schedule(8)
    clock.tick(8)
    assert clock.ticks == ticks + 128
    assert not clock.saturated


@pytest.mark.parametrize("clock_cls", [PureClock, NativeClock], ids=["pure", "native"])
def test_a_delay_is_rounded_half_up_to_a_tick_and_saturates(clock_cls):
    clock = clock_cls()
    alarm = clock.create_alarm(lambda: None)
    alarm.schedule(1 / 32)  # half a tick
    assert alarm.ticks == 1
    alarm.schedule(1 / 32 - 1 / 1000)
    assert alarm.ticks == 0
    alarm.schedule(float("nan"))
    assert alarm.ticks == 0
    alarm.schedule(1e30)  # "never": clamped, and the clock says so
    assert alarm.ticks == 2**62 - 1
    assert clock.saturated
    assert clock.nanos_to_next_alarm == (2**62 - 1) / 16


@pytest.mark.parametrize("clock_cls", [PureClock, NativeClock], ids=["pure", "native"])
def test_a_cycle_that_is_not_a_whole_number_of_ticks_does_not_drift(clock_cls):
    """133 MHz: 120.30 ticks per cycle. Rounded per cycle it would run 0.25 % fast; with the remainder carried, a million cycles, split any way, take the same time."""
    ticks_per_cycle = 1e9 / 133e6 * 16
    one_by_one, in_chunks = clock_cls(), clock_cls()
    for _ in range(1_000_000):
        one_by_one.advance_cycles(1, ticks_per_cycle)
    for _ in range(10_000):
        in_chunks.advance_cycles(100, ticks_per_cycle)
    exact = 1_000_000 * ticks_per_cycle
    assert abs(one_by_one.ticks - exact) <= 1
    assert abs(in_chunks.ticks - exact) <= 1
