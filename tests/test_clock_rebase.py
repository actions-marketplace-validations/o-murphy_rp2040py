"""The clock's origin moves (docs/records/0100-integer-clock.md).

An idle chip fast-forwards: with nothing waiting on the outside world the clock jumps from alarm to alarm, and a periodic alarm (the RTC's second) makes that millions of simulated seconds per wall
second - the whole `int64` in half a minute, which wedged the MicroPython `micropython/` tests. So when the time passes `REBASE_AT` the clock subtracts it from `now`, from every linked alarm and, through
listeners, from every absolute tick count a block holds; `nanos` (which carries the total) never goes back. These tests move the origin by hand in the middle of things and check that nothing a program can
see changes.
"""

import pytest
from utils.tick import start_tick

from rp2040py.clock._simulation_clock import REBASE_AT
from rp2040py.clock.mock_clock import MockClock
from rp2040py.clock.simulation_clock import SimulationClock

TIMERAWL = 0x40054028
TIMERRAWH = 0x40054024
ALARM0 = 0x40054010
INTR = 0x40054034
ROSC_COUNT = 0x40060020
XIP_STAT = 0x14000008
XIP_FLUSH = 0x14000004
PWM_CH0_CSR = 0x40050000
PWM_CH0_CTR = 0x40050008
PWM_EN = 0x400500A0


@pytest.fixture(params=[SimulationClock, MockClock], ids=["clock", "mock"])
def clock(request):
    return request.param()


def test_the_origin_moves_by_hand_and_nanos_does_not_go_back(clock):
    clock.tick_ticks(10_000)
    before = clock.nanos
    seen = []
    clock.add_rebase_listener(seen.append)
    clock.rebase(4_000)
    assert clock.ticks == 6_000 and clock.rebased_ticks == 4_000 and seen == [4_000]
    assert clock.nanos == before


def test_an_alarm_keeps_its_distance_and_a_never_alarm_stays_at_the_end(clock):
    fired = []
    near = clock.create_alarm(lambda: fired.append("near"))
    far = clock.create_alarm(lambda: fired.append("far"))
    never = clock.create_alarm(lambda: fired.append("never"))
    clock.tick_ticks(1_000)
    near.schedule_ticks(300)
    far.schedule_ticks(5_000)
    never.schedule(1e30)
    clock.rebase(700)
    assert clock.ticks_to_next_alarm == 300
    clock.tick_ticks(299)
    assert fired == []
    clock.tick_ticks(1)
    assert fired == ["near"]
    clock.tick_ticks(4_700)
    assert fired == ["near", "far"]
    assert never.ticks == 2**62 - 1  # not moved with the rest: it is still "never"


def test_it_moves_itself_when_the_time_passes_the_threshold(clock):
    moved = []
    clock.add_rebase_listener(moved.append)
    alarm_fired = []
    clock.create_alarm(lambda: alarm_fired.append(clock.ticks)).schedule_ticks(REBASE_AT + 500)
    clock.tick_ticks(REBASE_AT - 1)
    assert moved == []
    clock.tick_ticks(1)  # the time reaches the threshold: the origin moves to it
    assert moved == [REBASE_AT] and clock.ticks == 0 and clock.rebased_ticks == REBASE_AT
    clock.tick_ticks(500)
    assert alarm_fired == [500]  # it fired 500 ticks after the move, as it was due


def test_the_origin_does_not_move_from_inside_an_alarm(clock):
    """A callback that ticks the clock again must not have the alarm list taken from under the outer tick."""
    moved = []
    clock.add_rebase_listener(moved.append)
    inner = []

    def callback():
        inner.append(clock.ticks)
        clock.tick_ticks(REBASE_AT)  # a nested tick that would pass the threshold

    clock.create_alarm(callback).schedule_ticks(10)
    clock.tick_ticks(20)
    assert len(inner) == 1
    assert moved  # the move happened - after the outer tick, not during the callback


def test_a_long_idle_run_never_wedges(clock):
    """The failure itself: an alarm that re-arms every simulated second, jumped from alarm to alarm for far longer than the old range. The clock keeps going."""
    fires = []
    period = 10**15  # ns, about 11.6 days

    def tick():
        fires.append(1)
        alarm.schedule(period)

    alarm = clock.create_alarm(tick)
    alarm.schedule(period)
    for _ in range(1000):  # 1000 periods are 1.6e19 ticks, more than the old int64 range
        clock.tick_ticks(period * 16)
    assert len(fires) == 1000
    assert clock.rebased_ticks > 0 and clock.ticks < REBASE_AT * 2 and not clock.saturated


def move(chip, ticks=None):
    chip.clock.rebase(chip.clock.ticks if ticks is None else ticks)


def test_the_timer_counts_straight_through_a_move_of_the_origin(rp2040_factory):
    chip = rp2040_factory()
    start_tick(chip)
    chip.clock.tick(5_000)
    before = chip.read_uint32(TIMERAWL)
    chip.clock.rebase(chip.clock.ticks - 100)
    assert chip.read_uint32(TIMERAWL) == before
    chip.clock.tick(3_000)
    assert chip.read_uint32(TIMERAWL) == before + 3


def test_a_timer_alarm_armed_before_a_move_fires_on_time_after_it(rp2040_factory):
    chip = rp2040_factory()
    start_tick(chip)
    chip.clock.tick(1_000)
    now = chip.read_uint32(TIMERAWL)
    chip.write_uint32(ALARM0, now + 50)
    chip.clock.rebase(chip.clock.ticks - 10)
    chip.clock.tick(49_000)
    assert not chip.read_uint32(INTR) & 1
    chip.clock.tick(2_000)
    assert chip.read_uint32(INTR) & 1


def test_the_rosc_counter_counts_straight_through_a_move_of_the_origin(rp2040_factory):
    chip = rp2040_factory()
    chip.write_uint32(ROSC_COUNT, 200)
    chip.clock.tick(10_000)  # 6.5 MHz: 65 cycles
    before = chip.read_uint32(ROSC_COUNT)
    assert 0 < before < 200
    chip.clock.rebase(chip.clock.ticks - 50)
    assert chip.read_uint32(ROSC_COUNT) == before
    chip.clock.tick(10_000)
    assert chip.read_uint32(ROSC_COUNT) < before


def test_the_xip_flush_deadline_survives_a_move_of_the_origin(rp2040_factory):
    chip = rp2040_factory()
    chip.read_uint32(XIP_FLUSH)  # settle the flush the reset started
    chip.write_uint32(XIP_FLUSH, 1)
    assert not chip.read_uint32(XIP_STAT) & 1
    chip.clock.tick(4_000)
    chip.clock.rebase(chip.clock.ticks - 20)
    assert not chip.read_uint32(XIP_STAT) & 1  # not due yet: 1026 cycles are 8.2 us
    chip.clock.tick(5_000)
    assert chip.read_uint32(XIP_STAT) & 1


def test_a_pwm_counter_counts_straight_through_a_move_of_the_origin(rp2040_factory):
    chip = rp2040_factory()
    chip.write_uint32(PWM_EN, 1)
    chip.clock.tick(1_000)
    before = chip.read_uint32(PWM_CH0_CTR)
    assert before > 0
    chip.clock.rebase(chip.clock.ticks - 7)
    assert chip.read_uint32(PWM_CH0_CTR) == before
    chip.clock.tick(800)
    assert chip.read_uint32(PWM_CH0_CTR) == before + 100


def test_a_listener_that_is_a_bound_method_does_not_keep_its_holder_alive(clock):
    import gc
    import weakref

    class Holder:
        def on_rebase(self, delta):
            pass

    holder = Holder()
    clock.add_rebase_listener(holder.on_rebase)
    ref = weakref.ref(holder)
    del holder
    gc.collect()
    assert ref() is None
    clock.rebase(0)  # nothing to do, and a dead listener is simply dropped
    clock.tick_ticks(REBASE_AT)
    assert clock.rebased_ticks > 0


def test_a_block_that_holds_a_time_for_years_never_runs_away_from_the_clock(rp2040_factory):
    """The failure that remained after the first rebase fix (found on the MicroPython `extmod` sequence, ~3 minutes in): each move subtracted its `delta` from a block's stored epoch and never
    refreshed it, so after ~32 moves (9 simulated years) the epoch passed the int64 range and the TIMER read a negative time. The blocks fold their count at each move instead."""
    chip = rp2040_factory()
    start_tick(chip)
    expected = chip.read_uint32(TIMERAWL)
    for i in range(300):  # 100 simulated years
        step = REBASE_AT // 3 + i * 12345
        chip.clock.tick_ticks(step)
        expected += step / 16000.0
        raw = (chip.read_uint32(TIMERRAWH) << 32) | chip.read_uint32(TIMERAWL)
        assert abs(raw - expected) < 50, (i, raw, expected)
