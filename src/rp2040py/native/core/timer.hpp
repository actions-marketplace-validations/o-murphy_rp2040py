// The RP2040 TIMER block in C++ (docs/records/0096-cpp-mcu-core.md, Phase 2): a faithful translation of
// peripherals/timer.py, which stays as the pure-Python reference and the oracle.
//
// What it owns: the counter's epoch, the latched high word, INTR/INTE/INTF, PAUSE, and four alarms
// whose due times are scheduled on the C++ `Clock` (core/clock.hpp) - so a firmware's TIMELR poll loop, the
// hottest register traffic of a boot (52% of the accesses of a MicroPython boot, 97.6% of CircuitPython's), never
// leaves C++. What it does not own: logging, and the interrupt line - both are reached through the chip's `ChipPorts`
// (core/core_host.hpp), which also keeps this header free of anything Python.
//
// The count advances once per tick of the watchdog's tick generator (datasheet 4.6.4, 4.7.2): `tick_changed(hz)` is how the chip tells the block the tick's rate - nominally 1 MHz, a count per
// microsecond - and 0 means the tick is not running, which freezes the count exactly as PAUSE does (a bare chip does not count until firmware has started the tick, as on silicon). The count
// is continuous across a change of rate, and the armed alarms are re-timed. The count is an integer (64 bits, wrapping) plus the ticks of the clock into the count after it: the `n`th count after the last
// comes `ceil(n * period)` ticks later (16000 at 1 MHz, so exact integer arithmetic), and an alarm fires at the tick its count begins, never before (record 0105, finding 6).
//
// Every observable is kept, because replay of a recorded firmware session is the acceptance test: the number and
// order of interrupt-line calls (every INTR/INTE/INTF change and every alarm fire re-announces ALL FOUR lines, in
// order), which register accesses warn and with what, and the `raw_write_value` the alias-write path hands to INTR
// and ARMED.
//
// Header-only, C++17, no exceptions/RTTI/STL, no allocation. Single-threaded by contract.
#ifndef RP2040PY_CORE_TIMER_HPP
#define RP2040PY_CORE_TIMER_HPP

#include <cstdint>
#include <cstring>

#include "clock.hpp"
#include "core_host.hpp"
#include "window_map.hpp"

namespace rp2040core {


namespace timer_regs {
constexpr uint32_t TIMEHW = 0x00, TIMELW = 0x04, TIMEHR = 0x08, TIMELR = 0x0C, ALARM0 = 0x10, ALARM1 = 0x14, ALARM2 = 0x18, ALARM3 = 0x1C;
constexpr uint32_t ARMED = 0x20, TIMERAWH = 0x24, TIMERAWL = 0x28, DBGPAUSE = 0x2C, PAUSE = 0x30;
constexpr uint32_t kDbgPauseMask = 0x6, kDbgPauseReset = 0x6;  // DBG1 (bit 2) and DBG0 (bit 1), both 1 at reset; no debugger is ever attached, so they change nothing
constexpr uint32_t INTR = 0x34, INTE = 0x38, INTF = 0x3C, INTS = 0x40;
}  // namespace timer_regs

class TimerBlock : public RegisterBlock<TimerBlock> {
public:
    static constexpr int kAlarms = 4;
    static constexpr uint32_t kAlarmBits = 0xF;

    TimerBlock() = default;
    TimerBlock(const TimerBlock&) = delete;  // the alarm nodes point into this object
    TimerBlock& operator=(const TimerBlock&) = delete;

    // Binds the block to its clock and the chip's ports; `irq_line` is the NVIC number of TIMER_0 (the four follow). Must be called once, before anything else.
    void init(Clock* clock, const ChipPorts* ports, uint32_t block, uint32_t irq_line) noexcept {
        clock_ = clock;
        port_.init(ports, block, irq_line);
        clock_->add_rebase_listener(&rebase_listener_, &TimerBlock::on_rebase, this);  // the clock's origin moved: this block's epoch is an absolute tick count
        for (int i = 0; i < kAlarms; ++i) {
            slots_[i].block = this;
            slots_[i].index = i;
            alarms_[i].fire = &TimerBlock::on_alarm;
            alarms_[i].ctx = &slots_[i];
        }
    }

    // Unlinks every alarm from the clock; the owner calls it before the block goes away.
    void detach() noexcept {
        Clock::remove_rebase_listener(&rebase_listener_);
        for (int i = 0; i < kAlarms; ++i) cancel_alarm(&alarms_[i]);
    }

    uint32_t int_status() const noexcept { return (int_raw_ & int_enable_) | int_force_; }
    bool alarm_armed(int index) const noexcept { return armed_[index]; }

    // ---- registers: every "handled" return is false for an offset this block does not implement ----------

    bool read32(uint32_t offset, uint32_t* out) noexcept {
        using namespace timer_regs;
        const uint64_t time = count_now();
        switch (offset) {
            case TIMEHR: *out = static_cast<uint32_t>(latched_high_); return true;
            case TIMELR:
                latched_high_ = static_cast<int64_t>(time >> 32);
                *out = static_cast<uint32_t>(time);
                return true;
            case TIMERAWH: *out = static_cast<uint32_t>(time >> 32); return true;
            case TIMERAWL: *out = static_cast<uint32_t>(time); return true;
            case ALARM0: case ALARM1: case ALARM2: case ALARM3:
                *out = target_[(offset - ALARM0) / 4];
                return true;
            case PAUSE: *out = paused_ ? 1u : 0u; return true;
            case DBGPAUSE: *out = dbgpause_; return true;
            case INTR: *out = int_raw_; return true;
            case INTE: *out = int_enable_; return true;
            case INTF: *out = int_force_; return true;
            case INTS: *out = int_status(); return true;
            case ARMED: {
                uint32_t result = 0;
                for (int i = 0; i < kAlarms; ++i) {
                    if (armed_[i]) result |= 1u << i;
                }
                *out = result;
                return true;
            }
            default: return false;
        }
    }

    // What write32() reports. Plain ints, not an enum class, so the Cython shell can compare them directly.
    static constexpr int kWriteHandled = 0;
    static constexpr int kWriteUnhandled = 1;
    static constexpr int kWriteFailed = 2;

    // `value` is the value after any atomic-alias decode; `raw` is the value as the guest wrote it (INTR and ARMED
    // act on the raw bits, exactly as the Python block does through `raw_write_value`).
    int write32(uint32_t offset, int64_t value, int64_t raw) noexcept {
        using namespace timer_regs;
        switch (offset) {
            case ALARM0: case ALARM1: case ALARM2: case ALARM3: {
                const int index = static_cast<int>((offset - ALARM0) / 4);
                // The due time is computed from the unmasked value (as the Python block does); the register keeps 32 bits.
                armed_[index] = true;
                target_[index] = static_cast<uint32_t>(value);
                if (!frozen()) arm_clock_alarm(index);  // a frozen count: armed, and waiting for PAUSE to clear / the tick to run
                return kWriteHandled;
            }
            case ARMED:
                for (int i = 0; i < kAlarms; ++i) {
                    if (raw & (int64_t{1} << i)) disarm(i);
                }
                return kWriteHandled;
            case PAUSE:
                set_paused((value & 1) != 0);
                return kWriteHandled;
            case DBGPAUSE:
                dbgpause_ = static_cast<uint32_t>(value) & kDbgPauseMask;
                return kWriteHandled;
            case TIMELW:
                time_latch_low_ = static_cast<uint32_t>(value);
                return kWriteHandled;
            case TIMEHW:  // "always write timelw before timehw": this write sets the whole 64-bit time
                set_time((static_cast<uint64_t>(static_cast<uint32_t>(value)) << 32) | time_latch_low_);
                return kWriteHandled;
            case INTR:
                int_raw_ &= ~static_cast<uint32_t>(raw);
                return check_interrupts() ? kWriteHandled : kWriteFailed;
            case INTE:
                int_enable_ = static_cast<uint32_t>(value) & kAlarmBits;
                return check_interrupts() ? kWriteHandled : kWriteFailed;
            case INTF:
                int_force_ = static_cast<uint32_t>(value) & kAlarmBits;
                return check_interrupts() ? kWriteHandled : kWriteFailed;
            default: return kWriteUnhandled;
        }
    }

    // Registers and all four alarms back to power-on; the count restarts from zero by moving the epoch to now
    // (the simulation clock itself is never rewound). Returns false if an interrupt-line call failed.
    bool reset() noexcept {
        epoch_ticks_ = clock_->ticks();
        count_ = 0;
        sub_ = 0;
        latched_high_ = 0;
        int_raw_ = int_enable_ = int_force_ = 0;
        paused_ = false;
        // (the tick is not part of the block's registers: it keeps running (or not) through a reset)
        time_latch_low_ = 0;
        dbgpause_ = timer_regs::kDbgPauseReset;
        for (int i = 0; i < kAlarms; ++i) {
            disarm(i);
            target_[i] = 0;
        }
        for (int i = 0; i < kAlarms; ++i) {
            if (!raise_irq(i, false)) return false;
        }
        return true;
    }

    // ---- the register protocol as the bus (and the Python facade) uses it ---------------------------------

    // A read of any offset: an unimplemented one warns and reads as 0xFFFFFFFF, as `BasePeripheral` does.
    // A write of the value after the alias decode; the raw bits the INTR clear acts on are `raw_write_value()`.
    bool write(uint32_t offset, int64_t value) noexcept {
        const int result = write32(offset, value, raw_write_value_);
        if (result == kWriteUnhandled) port_.unimplemented_write(offset, value);
        return result != kWriteFailed;
    }

    uint32_t read(uint32_t offset) noexcept {
        uint32_t value;
        if (read32(offset, &value)) return value;
        port_.unimplemented_read(offset);
        return 0xFFFFFFFFu;
    }


    // ---- the watchdog's tick --------------------------------------------------------------------------------------

    // The tick is now `tick_hz` (0: stopped): the count continues from where it is at the new rate, or freezes, and the armed alarms are re-timed.
    void tick_changed(double tick_hz) noexcept {
        if (tick_hz != tick_hz_) apply(paused_, tick_hz);
    }
    double tick_hz() const noexcept { return tick_hz_; }
    bool paused() const noexcept { return paused_; }

    // ---- window handler entry points: what the bus's C++ window registry calls ---------------------------


private:
    struct Slot {
        TimerBlock* block = nullptr;
        int index = 0;
    };

    // The count does not advance: PAUSE is set, or the watchdog's tick is not running.
    bool frozen() const noexcept { return paused_ || tick_hz_ == 0.0; }
    // Ticks of the clock (1/16 ns) per count: 16000 at the nominal 1 MHz tick; not a whole number for a tick that clk_ref / CYCLES does not divide evenly.
    double period() const noexcept {
        const double p = static_cast<double>(kTicksPerNano) * 1e9 / tick_hz_;
        return p < 1.0 ? 1.0 : p;
    }
    // Ticks from a count to the `counts`th after it: ceil(counts * period), exact when the period is a whole number. (ceil without libm; the product is far below 2^53.)
    static int64_t ticks_of(int64_t counts, double period) noexcept {
        const int64_t whole = static_cast<int64_t>(period);
        if (static_cast<double>(whole) == period) return counts * whole;
        const double product = static_cast<double>(counts) * period;
        int64_t ticks = static_cast<int64_t>(product);  // truncates toward zero
        if (static_cast<double>(ticks) < product) ++ticks;
        return ticks;
    }
    // `total` ticks after a count: the counts that have passed, and (in `*sub`) the ticks since the last of them.
    int64_t split(int64_t total, int64_t* sub) const noexcept {
        const double p = period();
        const int64_t whole = static_cast<int64_t>(p);
        if (static_cast<double>(whole) == p) {
            *sub = total % whole;
            return total / whole;
        }
        int64_t counts = static_cast<int64_t>(static_cast<double>(total) / p);
        while (ticks_of(counts + 1, p) <= total) ++counts;
        while (counts > 0 && ticks_of(counts, p) > total) --counts;
        *sub = total - ticks_of(counts, p);
        return counts;
    }
    int64_t ticks_into_count() const noexcept { return sub_ + (clock_->ticks() - epoch_ticks_); }
    // The count (64 bits, wrapping), and in `*sub` the ticks since it last changed. Frozen while PAUSE is set or the tick is stopped.
    uint64_t count_and_sub(int64_t* sub) const noexcept {
        if (frozen()) {
            *sub = sub_;
            return count_;
        }
        const int64_t passed = split(ticks_into_count(), sub);
        return count_ + static_cast<uint64_t>(passed);
    }
    uint64_t count_now() const noexcept {
        int64_t sub;
        return count_and_sub(&sub);
    }
    // (Re)schedules alarm `index` for the moment the low 32 bits of the count reach its target, exactly: the tick at which that count begins (a target the count is already on fires at once).
    void arm_clock_alarm(int index) noexcept {
        const int64_t total = ticks_into_count();
        int64_t sub;
        const int64_t passed = split(total, &sub);
        const uint32_t counts = target_[index] - static_cast<uint32_t>(count_ + static_cast<uint64_t>(passed));
        clock_->schedule_ticks(&alarms_[index], counts == 0 ? 0 : ticks_of(passed + counts, period()) - total);
    }

    // PAUSE: the count freezes and no alarm can come due; clearing it resumes from the frozen count.
    void set_paused(bool paused) noexcept {
        if (paused != paused_) apply(paused, tick_hz_);
    }

    // Switch to a new PAUSE / tick state with the count continuous across it (the ticks into the current count are kept in proportion when the period changes).
    void apply(bool paused, double tick_hz) noexcept {
        const bool was_frozen = frozen();
        int64_t sub;
        const uint64_t count = count_and_sub(&sub);
        const double old_period = was_frozen ? 0.0 : period();
        paused_ = paused;
        tick_hz_ = tick_hz;
        if (frozen()) {
            count_ = count;
            sub_ = sub;
            if (!was_frozen) {
                for (int i = 0; i < kAlarms; ++i) clock_->cancel(&alarms_[i]);
            }
        } else {
            if (!was_frozen) sub = static_cast<int64_t>(static_cast<double>(sub) * period() / old_period);
            count_ = count;
            sub_ = sub;
            epoch_ticks_ = clock_->ticks();
            reschedule_armed_alarms();
        }
    }

    // The count is now `count`; every armed alarm is re-timed against it (an alarm matches the counter, not a moment of simulated time).
    void set_time(uint64_t count) noexcept {
        count_ = count;
        sub_ = 0;
        if (frozen()) return;
        epoch_ticks_ = clock_->ticks();
        reschedule_armed_alarms();
    }

    void reschedule_armed_alarms() noexcept {
        for (int i = 0; i < kAlarms; ++i) {
            if (armed_[i]) arm_clock_alarm(i);
        }
    }

    void disarm(int index) noexcept {
        clock_->cancel(&alarms_[index]);
        armed_[index] = false;
    }

    // Re-announces all four lines, in order, whatever changed - the Python block's exact call pattern.
    bool check_interrupts() noexcept {
        const uint32_t status = int_status();
        for (int i = 0; i < kAlarms; ++i) {
            if (!raise_irq(i, (status & (1u << i)) != 0)) return false;
        }
        return true;
    }

    static bool on_alarm(void* ctx) noexcept {
        Slot* slot = static_cast<Slot*>(ctx);
        TimerBlock* self = slot->block;
        self->disarm(slot->index);
        self->int_raw_ |= 1u << slot->index;
        return self->check_interrupts();
    }

    Clock* clock_ = nullptr;
    bool raise_irq(int alarm, bool level) noexcept { return port_.irq(level, static_cast<uint32_t>(alarm)); }
    static void on_rebase(void* ctx, int64_t delta) noexcept {
        auto* self = static_cast<TimerBlock*>(ctx);
        self->epoch_ticks_ -= delta;  // (the epoch is always near the clock's time, so this cannot run away)
        if (!self->frozen()) self->count_ = self->count_and_sub(&self->sub_);
        self->epoch_ticks_ = self->clock_->ticks();
    }

    RebaseListener rebase_listener_;
    int64_t epoch_ticks_ = 0;  // the clock's time, in ticks, at which this block's count was `count_` (plus `sub_` ticks)
    uint64_t count_ = 0;  // the count (64 bits, wrapping) at the epoch, or frozen
    int64_t sub_ = 0;  // the ticks of the clock into the count after it (0 <= sub_ < one period)
    // (both are folded into whenever the clock's origin moves, so the epoch never drifts away from the clock's time)
    int64_t latched_high_ = 0;
    uint32_t int_raw_ = 0, int_enable_ = 0, int_force_ = 0;
    bool paused_ = false;
    double tick_hz_ = 0.0;        // the watchdog's tick; 0 = not running, the count is frozen
    uint32_t time_latch_low_ = 0, dbgpause_ = timer_regs::kDbgPauseReset;
    bool armed_[kAlarms] = {false, false, false, false};
    uint32_t target_[kAlarms] = {0, 0, 0, 0};
    Alarm alarms_[kAlarms];
    Slot slots_[kAlarms];
};

}  // namespace rp2040core

#endif  // RP2040PY_CORE_TIMER_HPP
