// The simulated clock and alarm scheduler of the C++ MCU core (docs/records/0096-cpp-mcu-core.md, Phase 2; docs/records/0100-integer-clock.md).
//
// A faithful translation of clock/_simulation_clock.py (the pure-Python reference, kept as the oracle): time
// is an INTEGER count of ticks, advanced by `tick_ticks(delta)`, and alarms are a list sorted by due time that
// `tick` fires in order, setting the clock to each alarm's own time just before its callback runs.
//
// A tick is 1/16 ns (`kTicksPerNano`): an `int64_t` then holds 18 years of simulated time, exactly, where the `double` nanoseconds this
// replaced stopped resolving 8 ns beyond 104 days (the `rp2_dma` failure of the official suite). A 125 MHz cycle is exactly 128 ticks.
// The `double`-nanosecond entry points (`tick(delta)`, `link(alarm, delta)`, `nanos()`) stay for the blocks that think in
// nanoseconds: a delta is converted to ticks once, by `nanos_to_ticks` (round half up, saturating, NaN as 0), the same in the Python
// clocks. A per-cycle period that is not a whole number of ticks (133 MHz is 120.30 ticks) is carried as a remainder instead of rounded each time:
// `advance_cycles`.
//
// Allocation-free by construction: an `Alarm` is a node the CALLER owns (the Cython `ClockAlarm` embeds one),
// linked into the clock's list by pointer; the clock never allocates or frees. That puts one rule on the
// caller - an alarm must be unlinked (cancel) before its storage goes away - and the Cython wrapper keeps an
// alarm alive for exactly as long as it is linked.
//
// A callback is a plain function pointer + `ctx`, so a Python callable (through a Cython trampoline) and a C++
// block's own function are the same thing to the clock. It returns whether ticking may continue: `false` means
// "I failed and the failure is pending with the caller" - the Python trampoline cannot throw through this
// frame - and `tick` returns at once, leaving the clock at that alarm's time (exactly where an exception raised
// from the callback leaves the pure-Python clock).
//
// Header-only, C++17, no exceptions/RTTI/STL. Single-threaded by contract.
#ifndef RP2040PY_CORE_CLOCK_HPP
#define RP2040PY_CORE_CLOCK_HPP

#include <cmath>
#include <cstdint>

namespace rp2040core {

constexpr int64_t kTicksPerNano = 16;
constexpr int64_t kMaxTicks = INT64_MAX / 2;  // "never": times saturate here instead of wrapping (the clock records it, `saturated()`)

// A delta in nanoseconds as ticks: round half up, saturating at +-kMaxTicks, NaN as 0 - exactly `_simulation_clock.nanos_to_ticks`.
inline int64_t nanos_to_ticks(double nanos) noexcept {
    if (nanos != nanos) return 0;
    const double ticks = nanos * static_cast<double>(kTicksPerNano);
    if (ticks >= static_cast<double>(kMaxTicks)) return kMaxTicks;
    if (ticks <= -static_cast<double>(kMaxTicks)) return -kMaxTicks;
    return static_cast<int64_t>(std::floor(ticks + 0.5));
}

using AlarmFireFn = bool (*)(void* ctx);  // false: stop ticking, a failure is pending with the caller

class Clock;

struct Alarm {
    Alarm* next = nullptr;
    Clock* owner = nullptr;   // the clock whose list this alarm is linked into (null when not linked)
    int64_t ticks = 0;        // absolute due time while linked
    bool scheduled = false;   // linked into a clock's list
    AlarmFireFn fire = nullptr;
    void* ctx = nullptr;
};

class Clock {
public:
    double frequency = 125e6;

    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    // The clock may go away before the alarms linked into it (the garbage collector frees a cycle in no particular order): every
    // still-linked alarm is told it has no clock any more, so a later `cancel_alarm()` on it is a no-op instead of a write
    // through a dangling pointer. The alarm's owner needs no keep-alive reference to the clock for this.
    ~Clock() { reset(); }

    // Back to a freshly constructed clock: time 0, default frequency, no alarm linked (each one released as the destructor does).
    void reset() noexcept {
        while (head_ != nullptr) {
            Alarm* alarm = head_;
            head_ = alarm->next;
            alarm->next = nullptr;
            alarm->owner = nullptr;
            alarm->scheduled = false;
        }
        now_ = 0;
        frequency = 125e6;
        cycle_remainder_ = 0.0;
        saturated_ = false;
    }

    int64_t ticks() const noexcept { return now_; }
    // The time in nanoseconds, for display and for the blocks that think in them (inexact beyond 2^53 / 16 ns; differences of it should be taken in ticks).
    double nanos() const noexcept { return static_cast<double>(now_) / static_cast<double>(kTicksPerNano); }
    bool has_alarm() const noexcept { return head_ != nullptr; }
    // Time until the next alarm is due, or 0 when there is none (has_alarm() tells the two apart).
    int64_t ticks_to_next_alarm() const noexcept { return head_ != nullptr ? head_->ticks - now_ : 0; }
    double nanos_to_next_alarm() const noexcept { return static_cast<double>(ticks_to_next_alarm()) / static_cast<double>(kTicksPerNano); }
    // Whether a time was ever clamped to kMaxTicks (an alarm "never" due, or a jump beyond 146 years) since the last reset.
    bool saturated() const noexcept { return saturated_; }

    // Links `alarm` to fire `delta` nanoseconds from now. An alarm with the same due time as ones already in
    // the list goes AFTER them (FIFO): a zero-delay producer that reschedules itself must never cut in front
    // of a pending zero-delay consumer, or it can starve it (docs/records/0044).
    void link_ticks(Alarm* alarm, int64_t delta) noexcept {
        alarm->ticks = add_ticks(now_, delta);
        Alarm* item = head_;
        Alarm* last = nullptr;
        while (item != nullptr && item->ticks <= alarm->ticks) {
            last = item;
            item = item->next;
        }
        if (last != nullptr) {
            last->next = alarm;
        } else {
            head_ = alarm;
        }
        alarm->next = item;
        alarm->owner = this;
        alarm->scheduled = true;
    }

    // Removes `alarm` from the list if it is there; returns whether it was. Does not touch `scheduled`.
    bool unlink(Alarm* alarm) noexcept {
        Alarm* item = head_;
        Alarm* last = nullptr;
        while (item != nullptr) {
            if (item == alarm) {
                if (last != nullptr) {
                    last->next = item->next;
                } else {
                    head_ = item->next;
                }
                item->owner = nullptr;
                return true;
            }
            last = item;
            item = item->next;
        }
        return false;
    }

    void link(Alarm* alarm, double delta_nanos) noexcept { link_ticks(alarm, to_ticks(delta_nanos)); }

    // (Re)schedules: an alarm already linked is moved, not duplicated.
    void schedule_ticks(Alarm* alarm, int64_t delta) noexcept {
        if (alarm->scheduled) cancel(alarm);
        link_ticks(alarm, delta);
    }
    void schedule(Alarm* alarm, double delta_nanos) noexcept { schedule_ticks(alarm, to_ticks(delta_nanos)); }

    void cancel(Alarm* alarm) noexcept {
        unlink(alarm);
        alarm->scheduled = false;
    }

    // Advances time by `delta`, firing every alarm that comes due, in order, each at its own time. The list
    // is re-read after every callback (a callback may schedule or cancel anything). Returns false if a
    // callback asked to stop; the clock is then at that alarm's time and the rest of `delta` is not applied.
    bool tick_ticks(int64_t delta) noexcept {
        const int64_t target = add_ticks(now_, delta);
        while (head_ != nullptr && head_->ticks <= target) {
            Alarm* alarm = head_;
            head_ = alarm->next;
            alarm->owner = nullptr;
            alarm->scheduled = false;  // it is no longer in the list; a callback may re-arm it
            now_ = alarm->ticks;
            if (!alarm->fire(alarm->ctx)) return false;
        }
        now_ = target;
        return true;
    }
    bool tick(double delta_nanos) noexcept { return tick_ticks(to_ticks(delta_nanos)); }

    // Advances by `cycles` periods of `ticks_per_cycle` ticks each (not a whole number in general: 120.30 at 133 MHz) with the fraction CARRIED between calls, so
    // the long-run rate is exact however the cycles are split into calls - the CPU's time, one call per instruction.
    bool advance_cycles(int64_t cycles, double ticks_per_cycle) noexcept {
        cycle_remainder_ += static_cast<double>(cycles) * ticks_per_cycle;
        const double whole = std::floor(cycle_remainder_);
        cycle_remainder_ -= whole;
        return tick_ticks(whole >= static_cast<double>(kMaxTicks) ? kMaxTicks : static_cast<int64_t>(whole));
    }

private:
    // `nanos_to_ticks`, recording a clamp.
    int64_t to_ticks(double nanos) noexcept {
        const int64_t ticks = nanos_to_ticks(nanos);
        if (ticks == kMaxTicks || ticks == -kMaxTicks) saturated_ = true;
        return ticks;
    }

    int64_t add_ticks(int64_t a, int64_t b) noexcept {
        if (b > 0 && a > kMaxTicks - b) {
            saturated_ = true;
            return kMaxTicks;
        }
        return a + b;
    }

    int64_t now_ = 0;
    double cycle_remainder_ = 0.0;
    bool saturated_ = false;
    Alarm* head_ = nullptr;
};

// Unlinks `alarm` from whatever clock it is linked into, if that clock still exists. What an owner's `detach()` calls: unlike
// `clock->cancel(alarm)` it is safe when the clock was destroyed first.
inline void cancel_alarm(Alarm* alarm) noexcept {
    if (alarm->owner != nullptr) alarm->owner->cancel(alarm);
    alarm->scheduled = false;
}

}  // namespace rp2040core

#endif  // RP2040PY_CORE_CLOCK_HPP
