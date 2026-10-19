// The batch loop for two cores on one clock (docs/records/0104-second-core-seams-and-architecture.md, D5 and the scheduling decision of 2026-10-18: F2 main, A strict).
//
// `run_batch` (batch.hpp) is the loop of a chip whose core 1 is off and is not touched; this one serves the time core 1 is running, and `Mcu::run` chooses between them once per batch.
//
// Each core has its own local time. The cores run in quanta: from the clock's time to a horizon (a quantum of cycles, cut at the next alarm), core 0 and then core 1 execute instructions
// until their local time reaches the horizon, and the clock is ticked to the earlier of the two. A quantum alone would let a core see another's data a quantum late, so every access that leaves
// RAM and flash - the SIO, the PPB, every peripheral - is a SYNCHRONISATION POINT (`Bus::set_sync`): before the access happens the other core, if it is behind, executes up to this core's time
// (a nested catch-up), and the clock and the PIO are brought to this core's time, so the access is ordered by time exactly as the exact scheduler A would order it. Data two cores exchange through
// RAM or flash with no such access between is seen up to one quantum late (the accuracy contract of the record). `strict` makes the horizon one cycle: the exact scheduler A.
//
// Alarms, the PIO and the DMA are advanced by the CLOCK's delta (the PIO by whole cycles of it, the fraction carried), not by one core's cycles.
//
// Header-only, C++17, no exceptions/RTTI/STL, no allocation.
#ifndef RP2040PY_CORE_BATCH_DUAL_HPP
#define RP2040PY_CORE_BATCH_DUAL_HPP

#include <cmath>
#include <cstdint>
#include <limits>

#include "batch.hpp"
#include "bus.hpp"

namespace rp2040core {

// Where `LR` points when core 1 starts: the pico-sdk's `core1_trampoline` leaves the boot ROM's return address there on purpose, so that an entry function that returns lands back in the ROM, in
// `_wait_for_vector` (datasheet 2.8.1.1, 2.8.2: core 1 waits for a launch there after reset). The boot ROM is not run for core 1 (the SIO answers its handshake), so this is a made-up address in the last
// word of the ROM's 16 KiB, which no ROM function starts at: reaching it parks core 1.
constexpr uint32_t kCore1RomReturn = 0x00003FFCu;

namespace dual_detail {

struct State {
    Cpu* cpu[2] = {nullptr, nullptr};
    Bus* bus = nullptr;
    Clock* clock = nullptr;
    const BatchHost* host = nullptr;
    int64_t local[2] = {0, 0};     // each core's own time, in clock ticks
    int64_t wake_at[2] = {0, 0};   // the earliest time a core may run again: a core woken by the other (a FIFO word, a SEV) or by an alarm did not run before it
    double frac[2] = {0.0, 0.0};   // the fraction of a tick a cycle that is not a whole number of ticks leaves, per core
    double pio_carry = 0.0;        // clock ticks not yet handed to the PIO as whole cycles
    double ticks_per_cycle = 128.0;
    int64_t cycle_ticks = 128;     // 0: not a whole number
    int64_t horizon = 0;
    long iterations = 0;           // instructions executed, of either core
    uint8_t current = 0;           // the core whose instruction is running
    bool failed = false;
    bool parked = false;           // core 1 returned from its entry function to the boot ROM (`kCore1RomReturn`): the batch ends, `Mcu::run` puts it back to the ROM's wait
};

inline int64_t ticks_for(State& s, int core, int64_t cycles) noexcept {
    if (s.cycle_ticks != 0) return cycles * s.cycle_ticks;
    s.frac[core] += static_cast<double>(cycles) * s.ticks_per_cycle;
    const double whole = std::floor(s.frac[core]);
    s.frac[core] -= whole;
    return static_cast<int64_t>(whole);
}

// Hands the PIO blocks the whole cycles `ticks` of the clock cover.
inline bool step_pio(State& s, int64_t ticks) noexcept {
    const BatchHost& host = *s.host;
    if (host.pio_count == 0) return true;
    s.pio_carry += static_cast<double>(ticks);
    const int64_t cycles = static_cast<int64_t>(s.pio_carry / s.ticks_per_cycle);
    if (cycles <= 0) return true;
    s.pio_carry -= static_cast<double>(cycles) * s.ticks_per_cycle;
    for (int p = 0; p < host.pio_count; ++p) {
        if (!*host.pio_stopped[p] && !host.pio_advance(host.ctx, p, cycles)) return false;
    }
    return true;
}

// The clock (and with it every alarm and the PIO) to time `t`.
inline bool advance_to(State& s, int64_t t) noexcept {
    const int64_t dt = t - s.clock->ticks();
    if (dt <= 0) return true;
    if (!s.clock->tick_ticks(dt)) return false;
    return step_pio(s, dt);
}

inline void note_wake(State& s) noexcept {
    for (int i = 0; i < 2; ++i) {
        if (s.wake_at[i] < s.clock->ticks()) s.wake_at[i] = s.clock->ticks();
    }
}

// One instruction of `core`. False: it failed (the host has parked the error).
inline bool execute_one(State& s, int core) noexcept {
    if (core == 1 && (s.cpu[1]->registers[15] & ~1u) == kCore1RomReturn) {  // (the entry function returned: what the ROM does next is `_wait_for_vector`, the state of a core 1 that was just reset)
        s.cpu[1]->waiting = true;
        s.parked = true;
        return true;
    }
    const uint8_t saved = s.current;
    s.current = static_cast<uint8_t>(core);
    s.bus->initiator = static_cast<uint8_t>(core);
    const int cycles = s.cpu[core]->execute();
    s.current = saved;
    s.bus->initiator = saved;
    if (cycles < 0 || s.failed) return false;
    s.local[core] += ticks_for(s, core, cycles);
    ++s.iterations;
    return true;
}

// The synchronisation point (`Bus::set_sync`): runs inside a device access of core `current`, before the access. The other core catches up to this core's time first, then the clock and the PIO
// are brought to it, so the access sees the chip as it is at that time. A failure is parked with the host; `execute()` returns it on the access's own check.
inline void sync(void* ctx) noexcept {
    State& s = *static_cast<State*>(ctx);
    const int me = s.current, other = 1 - me;
    if (!s.cpu[other]->waiting) {
        if (s.local[other] < s.wake_at[other]) s.local[other] = s.wake_at[other];
        while (s.local[other] < s.local[me] && !s.cpu[other]->waiting && !s.failed) {
            if (!execute_one(s, other)) {
                s.failed = true;
                return;
            }
        }
    }
    if (!advance_to(s, s.local[me])) s.failed = true;
    if (s.clock->has_alarm()) {
        const int64_t next = s.clock->ticks() + s.clock->ticks_to_next_alarm();
        if (next < s.horizon) s.horizon = next;
    }
    note_wake(s);
    // The access itself may wake the other core (a FIFO word, a SEV): it cannot have run before now.
    if (s.wake_at[other] < s.local[me]) s.wake_at[other] = s.local[me];
}

}  // namespace dual_detail

// Runs one batch of two cores. `result` and the return value are as for `run_batch`; `iterations` counts instructions of either core and the ceiling bounds their sum.
inline int run_batch_dual(Cpu& cpu0, Cpu& cpu1, Bus& bus, Clock& clock, const BatchHost& host, const BatchParams& params, BatchResult* result = nullptr) noexcept {
    using namespace dual_detail;
    constexpr int64_t kNoAlarm = std::numeric_limits<int64_t>::max();
    State s;
    s.cpu[0] = &cpu0;
    s.cpu[1] = &cpu1;
    s.bus = &bus;
    s.clock = &clock;
    s.host = &host;
    s.ticks_per_cycle = params.cycle_nanos * static_cast<double>(kTicksPerNano);
    const bool whole = s.ticks_per_cycle == std::floor(s.ticks_per_cycle) && s.ticks_per_cycle < 1e12;
    s.cycle_ticks = whole ? static_cast<int64_t>(s.ticks_per_cycle) : 0;
    const int64_t quantum = (params.quantum_cycles < 1 ? 1 : params.quantum_cycles) * (whole ? s.cycle_ticks : static_cast<int64_t>(s.ticks_per_cycle));
    const int64_t start_ticks = clock.ticks();
    s.local[0] = s.local[1] = s.wake_at[0] = s.wake_at[1] = start_ticks;
    int stop_reason = kStopCeiling;
    int ticks_since_check = 0;
    const double batch_start = host.monotonic(host.ctx);
    SyncedHandler storage[Bus::kSyncStorage];  // (the wrappers of the device handlers: they live for this batch)
    bus.set_sync(&sync, &s, storage);
    auto done = [&](int status, int reason) noexcept {
        bus.set_sync(nullptr, nullptr, nullptr);
        bus.initiator = 0;
        if (result != nullptr) {
            result->reason = reason;
            result->iterations = s.iterations;
            result->ticks_advanced = clock.ticks() - start_ticks;
        }
        return status;
    };

    while (s.iterations < params.instruction_ceiling && !s.parked && !*host.stopped && !(host.clk_changed != nullptr && *host.clk_changed != 0)) {
        if (++ticks_since_check >= params.check_interval) {
            ticks_since_check = 0;
            if (host.monotonic(host.ctx) - batch_start > params.yield_budget_seconds) {
                stop_reason = kStopBudget;
                break;
            }
        }
        const int64_t now = clock.ticks();
        if (cpu0.waiting && cpu1.waiting) {  // both asleep: jump to the next alarm, as the single-core loop does (the PIO gets the cycles it covers, at least one)
            int64_t idle_ticks = clock.has_alarm() ? clock.ticks_to_next_alarm() : 0;
            bool paced = false;
            if (host.real_io != nullptr && *host.real_io != 0) {
                const int64_t cap = nanos_to_ticks(params.paced_idle_nanos);
                if (idle_ticks > cap) idle_ticks = cap;
                paced = true;
            }
            if (!clock.tick_ticks(idle_ticks)) return done(kBatchFault, kStopFault);
            if (!step_pio(s, idle_ticks > 0 ? idle_ticks : static_cast<int64_t>(s.ticks_per_cycle))) return done(kBatchFault, kStopFault);
            note_wake(s);
            ++s.iterations;
            if (paced) {
                stop_reason = kStopPaced;
                break;
            }
            continue;
        }
        // A quantum: to the horizon, cut at the next alarm, at least one cycle long.
        s.horizon = now + quantum;
        if (clock.has_alarm()) {
            const int64_t next = now + clock.ticks_to_next_alarm();
            if (next < s.horizon) s.horizon = next;
        }
        if (s.horizon < now + (whole ? s.cycle_ticks : 1)) s.horizon = now + (whole ? s.cycle_ticks : 1);
        for (int core = 0; core < 2; ++core) {
            if (s.cpu[core]->waiting) continue;
            if (s.local[core] < now) s.local[core] = now;
            if (s.local[core] < s.wake_at[core]) s.local[core] = s.wake_at[core];
            while (s.local[core] < s.horizon && !s.cpu[core]->waiting && s.iterations < params.instruction_ceiling) {
                if (!execute_one(s, core)) return done(kBatchFault, kStopFault);
            }
        }
        // The clock goes to the earlier of the cores still running (a core that fell asleep is not waited for); if both did, to the later of the two sleeps.
        int64_t t = kNoAlarm;
        for (int core = 0; core < 2; ++core) {
            if (!s.cpu[core]->waiting && s.local[core] < t) t = s.local[core];
        }
        if (t == kNoAlarm) t = s.local[0] > s.local[1] ? s.local[0] : s.local[1];
        if (!advance_to(s, t)) return done(kBatchFault, kStopFault);
        note_wake(s);
        if (s.failed) return done(kBatchFault, kStopFault);
    }
    if (stop_reason == kStopCeiling && s.iterations < params.instruction_ceiling) {  // the loop condition ended it
        stop_reason = *host.stopped ? kStopRequested : kStopClockChanged;
    }
    return done(0, stop_reason);
}

}  // namespace rp2040core

#endif  // RP2040PY_CORE_BATCH_DUAL_HPP
