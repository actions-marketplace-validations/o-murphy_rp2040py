// Standalone checks of src/rp2040py/native/core/batch.hpp (see tests/test_core_cpp.py for the flags).
#include <cstdio>
#include <cstring>

#include "batch.hpp"

using namespace rp2040core;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static uint8_t sram[4096];
static void put16(uint32_t addr, uint16_t v) { std::memcpy(sram + (addr - 0x20000000), &v, 2); }

static double fake_now = 0.0, fake_step = 0.0;
static double monotonic(void*) noexcept { return fake_now += fake_step; }
static int pio_calls = 0;
static int64_t pio_cycles = 0;
static bool pio_advance(void*, int, int64_t cycles) noexcept {
    ++pio_calls;
    pio_cycles += cycles;
    return true;
}

static int alarm_fired = 0;
static double alarm_time = 0;
static bool alarm_fire(void*) noexcept {
    ++alarm_fired;
    return true;
}

static Clock* rearm_clock = nullptr;
static Alarm* rearm_alarm = nullptr;
static bool alarm_rearm(void*) noexcept {
    rearm_clock->link_ticks(rearm_alarm, 1000);  // 62.5 ns
    return true;
}

int main() {
    Bus bus;
    bus.mem.attach({0x20000000, 4096, 4096, 0xFFFFFFFF, sram, kSubWord});
    Cpu cpu;
    cpu.init(&bus, {}, 25);
    Clock clock;
    // A tight loop of nops (0xBF00) ending in a branch back: b .-6  (0xE7FB)
    for (uint32_t a = 0; a < 6; a += 2) put16(0x20000000 + a, 0xBF00);
    put16(0x20000006, 0xE7FB);
    cpu.registers[15] = 0x20000000;

    uint8_t stopped = 0;
    int pio_stopped = 0;
    BatchHost host;
    host.stopped = &stopped;
    host.pio_count = 1;
    host.pio_stopped[0] = &pio_stopped;
    host.pio_advance = pio_advance;
    host.monotonic = monotonic;
    BatchParams params;
    params.instruction_ceiling = 1000;

    // The ceiling bounds the batch; time advances by each instruction's cycles (nop 1, branch 2) and the running PIO gets every one.
    CHECK(run_batch(cpu, clock, host, params) == 0);
    CHECK(cpu.cycles > 1000 && clock.nanos() == static_cast<double>(cpu.cycles) * params.cycle_nanos);
    CHECK(pio_calls == 1000 && static_cast<uint64_t>(pio_cycles) == cpu.cycles);

    // A stopped PIO is not called; a stop request ends the batch before any instruction.
    pio_calls = 0;
    pio_stopped = 1;
    const uint64_t before = cpu.cycles;
    CHECK(run_batch(cpu, clock, host, params) == 0 && pio_calls == 0 && cpu.cycles > before);
    stopped = 1;
    const uint64_t at_stop = cpu.cycles;
    CHECK(run_batch(cpu, clock, host, params) == 0 && cpu.cycles == at_stop);
    stopped = 0;

    // `clk_changed` raised: the clock tree moved clk_sys, the period in `params` is stale, so the batch ends before another instruction (the caller starts the next one at the new period);
    // lowered (or absent), it changes nothing.
    uint8_t clk_changed = 1;
    host.clk_changed = &clk_changed;
    CHECK(run_batch(cpu, clock, host, params) == 0 && cpu.cycles == at_stop);
    clk_changed = 0;
    CHECK(run_batch(cpu, clock, host, params) == 0 && cpu.cycles > at_stop);
    host.clk_changed = nullptr;

    // The time an instruction costs is the period the caller passes: at 250 MHz (4 ns) the same instructions take half the simulated time they take at 125 MHz (8 ns).
    {
        Cpu fast, slow;
        fast.init(&bus, {}, 25);
        slow.init(&bus, {}, 25);
        fast.registers[15] = slow.registers[15] = 0x20000000;
        Clock fast_clock, slow_clock;
        BatchParams fast_params = params, slow_params = params;
        fast_params.cycle_nanos = 1e9 / 250e6;
        slow_params.cycle_nanos = 1e9 / 125e6;
        fast_params.instruction_ceiling = slow_params.instruction_ceiling = 100;
        CHECK(run_batch(fast, fast_clock, host, fast_params) == 0 && run_batch(slow, slow_clock, host, slow_params) == 0);
        CHECK(fast.cycles == slow.cycles && fast_clock.nanos() * 2 == slow_clock.nanos() && fast_clock.nanos() > 0);
    }

    // Batched ticking reaches the same simulated time, and an alarm due mid-batch fires at the same point.
    Cpu cpu2;
    cpu2.init(&bus, {}, 25);
    cpu2.registers[15] = 0x20000000;
    Clock clock2;
    Alarm alarm;
    alarm.fire = alarm_fire;
    clock2.link(&alarm, 500.0);  // 500 ns
    params.tick_batch = 16;
    pio_stopped = 1;
    CHECK(run_batch(cpu2, clock2, host, params) == 0);
    CHECK(alarm_fired == 1 && clock2.nanos() == static_cast<double>(cpu2.cycles) * params.cycle_nanos);
    (void)alarm_time;

    // An idle core jumps straight to the next alarm, and the PIO is handed the clocks the jump covered (at least one).
    Cpu cpu3;
    cpu3.init(&bus, {}, 25);
    cpu3.waiting = true;
    Clock clock3;
    Alarm alarm3;
    alarm3.fire = alarm_fire;
    clock3.link(&alarm3, 8000.0);
    pio_stopped = 0;
    pio_calls = 0;
    pio_cycles = 0;
    params.tick_batch = 1;
    params.instruction_ceiling = 1;
    alarm_fired = 0;
    CHECK(run_batch(cpu3, clock3, host, params) == 0 && alarm_fired == 1 && clock3.nanos() == 8000.0);
    CHECK(pio_calls == 1 && pio_cycles == 1000);
    // Nothing scheduled: time does not move, the PIO still gets one clock.
    pio_calls = 0;
    pio_cycles = 0;
    CHECK(run_batch(cpu3, clock3, host, params) == 0 && clock3.nanos() == 8000.0 && pio_calls == 1 && pio_cycles == 1);

    // A clock that is not a whole number of ticks per cycle (133 MHz: 120.30): many short idle jumps hand the PIO the cycles they cover IN TOTAL, the fraction of each carried to the next
    // (flooring every jump on its own gave 8 of 8.31 cycles per jump, and the PIO ran 4 % slow against the clock - record 0105).
    Cpu cpu5;
    cpu5.init(&bus, {}, 25);
    cpu5.waiting = true;
    Clock clock5;
    Alarm alarm5;
    alarm5.fire = alarm_rearm;
    rearm_clock = &clock5;
    rearm_alarm = &alarm5;
    clock5.link_ticks(&alarm5, 1000);
    BatchParams params5;
    params5.cycle_nanos = 1e9 / 133e6;
    params5.instruction_ceiling = 1000;
    pio_stopped = 0;
    pio_calls = 0;
    pio_cycles = 0;
    CHECK(run_batch(cpu5, clock5, host, params5) == 0);
    const double expected = static_cast<double>(clock5.ticks()) / (params5.cycle_nanos * static_cast<double>(kTicksPerNano));
    CHECK(clock5.ticks() == 1000 * 1000);
    CHECK(static_cast<double>(pio_cycles) > expected - 1.0 && static_cast<double>(pio_cycles) <= expected);

    // The real-time budget ends a batch early (checked every `check_interval` iterations).
    Cpu cpu4;
    cpu4.init(&bus, {}, 25);
    cpu4.registers[15] = 0x20000000;
    Clock clock4;
    params.instruction_ceiling = 1000000;
    params.check_interval = 4;
    params.yield_budget_seconds = 0.5;
    fake_now = 0.0;
    fake_step = 0.2;  // every look at the clock is 0.2 s later
    pio_stopped = 1;
    CHECK(run_batch(cpu4, clock4, host, params) == 0 && cpu4.cycles < 40);

    if (failures == 0) std::printf("batch: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
