// Standalone checks of src/rp2040py/native/core/mcu.hpp and of the result `run_batch` gives (see tests/test_core_cpp.py for the flags).
#include <cstdio>
#include <cstring>

#include "mcu.hpp"

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

static bool alarm_fail(void*) noexcept { return false; }

int main() {
    Mcu mcu;
    mcu.bus.mem.attach({0x20000000, 4096, 4096, 0xFFFFFFFF, sram, kSubWord});
    for (uint32_t a = 0; a < 6; a += 2) put16(0x20000000 + a, 0xBF00);  // nop, nop, nop
    put16(0x20000006, 0xE7FB);                                          // b .-6
    mcu.cpu.init(&mcu.bus, {}, 25);
    mcu.cpu.registers[15] = 0x20000000;
    Cpu& cpu = mcu.cpu;
    Clock& clock = mcu.clock();

    mcu.set_monotonic(monotonic);
    BatchParams limits;
    limits.instruction_ceiling = 1000;

    McuResult r;

    // Built with no clock the chip runs on its own; a clock handed to it is used where it is, and `use_clock(nullptr)` goes back to the owned one.
    CHECK(mcu.owns_clock() && &mcu.clock() == &clock);
    {
        Clock given;
        mcu.use_clock(&given);
        CHECK(!mcu.owns_clock() && &mcu.clock() == &given);
        mcu.use_clock(nullptr);
        CHECK(mcu.owns_clock() && &mcu.clock() == &clock);
    }

    // The ceiling ends it; the result says how many iterations and how much simulated time (ticks), which is what the clock advanced by.
    const int64_t t0 = clock.ticks();
    CHECK(mcu.run(limits, &r) == 0);
    CHECK(r.reason == kStopCeiling && r.iterations == 1000);
    CHECK(r.ticks_advanced == clock.ticks() - t0 && r.ticks_advanced > 0);
    CHECK(clock.nanos() == static_cast<double>(cpu.cycles) * limits.cycle_nanos);

    // The result is optional, and a run without one does the same thing.
    const uint64_t cycles_before = cpu.cycles;
    CHECK(mcu.run(limits) == 0 && cpu.cycles > cycles_before);

    // A stop request: no iteration, no time.
    CHECK(!mcu.stop_requested());
    mcu.request_stop();
    CHECK(mcu.stop_requested() && *mcu.stop_address() == 1);
    const uint64_t at_stop = cpu.cycles;
    CHECK(mcu.run(limits, &r) == 0 && r.reason == kStopRequested && r.iterations == 0 && r.ticks_advanced == 0 && cpu.cycles == at_stop);
    mcu.request_stop(false);
    CHECK(!mcu.stop_requested() && *mcu.stop_address() == 0);

    // The clock tree moved clk_sys: the batch ends before an instruction and says why.
    uint8_t clk_changed = 1;
    mcu.set_clk_changed_flag(&clk_changed);
    CHECK(mcu.run(limits, &r) == 0 && r.reason == kStopClockChanged && r.iterations == 0 && cpu.cycles == at_stop);
    clk_changed = 0;
    mcu.set_clk_changed_flag(nullptr);

    // The real-time budget: the monotonic clock jumps past it at the first check (every `check_interval` iterations).
    {
        BatchParams budget = limits;
        budget.check_interval = 8;
        budget.yield_budget_seconds = 0.005;
        fake_now = 0.0;
        fake_step = 1.0;  // each reading is a second later than the last
        CHECK(mcu.run(budget, &r) == 0 && r.reason == kStopBudget && r.iterations > 0 && r.iterations < 1000);
        fake_step = 0.0;
    }

    // An idle core jumps to the next alarm; while a device waits on the real world the jump is capped and the batch ends there (the host sleeps for the time it covered).
    {
        Mcu m;
        m.set_monotonic(monotonic);
        m.cpu.init(&m.bus, {}, 25);
        m.cpu.waiting = true;
        Alarm far;
        far.fire = [](void*) noexcept { return true; };
        m.clock().link(&far, 50e6);  // 50 ms away
        m.set_real_io(true);
        CHECK(m.real_io() && *m.real_io_address() == 1);
        CHECK(m.run(limits, &r) == 0 && r.reason == kStopPaced && r.iterations == 1);
        CHECK(r.ticks_advanced == nanos_to_ticks(limits.paced_idle_nanos) && m.clock().nanos() == limits.paced_idle_nanos);
        // without the wait the same jump goes the whole way to the alarm
        m.set_real_io(false);
        CHECK(m.run(limits, &r) == 0 && r.reason == kStopCeiling && m.clock().nanos() >= 50e6);  // (then the idle iterations with no alarm left run out the ceiling)
    }

    // A failing clock alarm is a fault: the result says so and the batch ends.
    {
        Mcu m;
        m.bus.mem.attach({0x20000000, 4096, 4096, 0xFFFFFFFF, sram, kSubWord});
        m.set_monotonic(monotonic);
        m.cpu.init(&m.bus, {}, 25);
        m.cpu.registers[15] = 0x20000000;
        Alarm bad;
        bad.fire = alarm_fail;
        m.clock().link(&bad, 8.0);  // due after the first instruction
        CHECK(m.run(limits, &r) == kBatchFault && r.reason == kStopFault);
    }

    // The interrupt lines: `set_interrupt` reaches the CPU, a hook takes the line instead of the CPU, and taking the hook away gives it back.
    {
        struct Seen {
            int n = 0;
            uint32_t irq = 0;
            bool level = false;
            bool result = true;
        } seen;
        Mcu m;
        m.cpu.init(&m.bus, {}, 25);
        CHECK(m.set_interrupt(5, true) && (m.cpu.pending_interrupts & (1u << 5)) != 0);
        CHECK(m.set_interrupt(5, false) && (m.cpu.pending_interrupts & (1u << 5)) == 0);
        m.set_irq_hook(
            [](void* ctx, uint32_t irq, bool level) noexcept {
                Seen* s = static_cast<Seen*>(ctx);
                ++s->n;
                s->irq = irq;
                s->level = level;
                return s->result;
            },
            &seen);
        CHECK(m.set_interrupt(7, true) && seen.n == 1 && seen.irq == 7 && seen.level);
        CHECK((m.cpu.pending_interrupts & (1u << 7)) == 0);  // the hook got the line instead of the CPU
        seen.result = false;
        CHECK(!m.set_interrupt(8, true));  // its failure is the caller's
        m.set_irq_hook(nullptr, nullptr);
        CHECK(m.set_interrupt(7, true) && seen.n == 2 && (m.cpu.pending_interrupts & (1u << 7)) != 0);

        // The adapters a shell hands a block: a fixed line, a line offset from a base, and IO_BANK0's, which is the pin bank's "any IRQ value".
        McuIrqLine fixed;
        fixed.mcu = &m;
        fixed.line = 20;
        CHECK(mcu_irq_line(&fixed, true) && (m.cpu.pending_interrupts & (1u << 20)) != 0);
        CHECK(mcu_irq_line(&fixed, false) && (m.cpu.pending_interrupts & (1u << 20)) == 0);
        McuIrqLines based;
        based.mcu = &m;
        based.base = 4;
        CHECK(mcu_irq_lines(&based, 3, true) && (m.cpu.pending_interrupts & (1u << 7)) != 0);
        PinBank bank;
        PinHost no_host;
        CHECK(bank.init(30, no_host));
        McuIoIrq io;
        io.mcu = &m;
        io.bank = &bank;
        io.line = 13;
        CHECK(mcu_io_interrupt(&io) && (m.cpu.pending_interrupts & (1u << 13)) == 0);  // nothing raises it
        bank.pin(3).irq_force_mask = 1;
        CHECK(mcu_io_interrupt(&io) && (m.cpu.pending_interrupts & (1u << 13)) != 0);  // one pin does
        bank.pin(3).irq_force_mask = 0;
        CHECK(mcu_io_interrupt(&io) && (m.cpu.pending_interrupts & (1u << 13)) == 0);  // and the line follows it down
    }

    // The DMA requests: `set_dreq` reaches the attached DMA block, a hook takes the request instead of it, none attached goes nowhere, and the adapters map a block's own shape onto
    // the absolute DREQ number.
    {
        Mcu m;
        Clock clock;
        DmaHost dma_host;
        DmaBlock dma;
        dma.init(&m.bus, &clock, dma_host);
        CHECK(m.set_dreq(5, true) && !dma.dreq(5));  // no DMA attached: nowhere to go, not a failure
        m.attach_dma(&dma);
        CHECK(m.set_dreq(5, true) && dma.dreq(5));
        CHECK(m.set_dreq(5, false) && !dma.dreq(5));

        McuDreqLine line;
        line.mcu = &m;
        line.number = 36;
        CHECK(mcu_dreq_line(&line, true) && dma.dreq(36));
        CHECK(mcu_dreq_line(&line, false) && !dma.dreq(36));

        McuDreqPair pair;
        pair.mcu = &m;
        pair.if_true = 20;
        pair.if_false = 21;
        CHECK(mcu_dreq_pair(&pair, true, true) && dma.dreq(20) && !dma.dreq(21));
        CHECK(mcu_dreq_pair(&pair, false, true) && dma.dreq(21));
        CHECK(mcu_dreq_pair(&pair, true, false) && !dma.dreq(20) && dma.dreq(21));

        McuDreqBase base;
        base.mcu = &m;
        base.base = 24;
        CHECK(mcu_dreq_base(&base, 3) && dma.dreq(27));  // the PWM only ever asserts (a wrap)

        CHECK(mcu_dreq_number(&m, 9, true) && dma.dreq(9) && mcu_dreq_number(&m, 9, false) && !dma.dreq(9));  // the PIO: the number is already absolute

        struct Seen {
            int n = 0;
            uint32_t number = 0;
            bool asserted = false;
            bool result = true;
        } seen;
        m.set_dreq_hook(
            [](void* ctx, uint32_t number, bool asserted) noexcept {
                Seen* x = static_cast<Seen*>(ctx);
                ++x->n;
                x->number = number;
                x->asserted = asserted;
                return x->result;
            },
            &seen);
        CHECK(m.set_dreq(12, true) && seen.n == 1 && seen.number == 12 && seen.asserted && !dma.dreq(12));  // the hook got it instead of the DMA
        seen.result = false;
        CHECK(!m.set_dreq(13, true));  // its failure is the caller's
        m.set_dreq_hook(nullptr, nullptr);
        CHECK(m.set_dreq(12, true) && seen.n == 2 && dma.dreq(12));
        m.attach_dma(nullptr);
        CHECK(m.set_dreq(14, true) && !dma.dreq(14));
    }

    if (failures == 0) std::printf("test_mcu: ok\n");
    return failures == 0 ? 0 : 1;
}
