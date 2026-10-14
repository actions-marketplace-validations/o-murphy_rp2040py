// The chip as one object (docs/records/0096-cpp-mcu-core.md, step 2a of the plan of 2026-10-14): what a host asks of the core, instead of a host reaching for the CPU, the bus,
// the clock and the batch loop one by one. `Mcu` owns the CPU, the bus, the two PIO blocks (the loop steps them), the run flags and the host of the batch loop, and has the
// step API, `run()`; the other blocks, the reset cascade, the wiring and the host interface proper (the world in, the callbacks out) come in the steps after it, each its own
// go-ahead (3a: one block per commit; the PIO goes first because the loop depends on it).
//
// **The clock is the one thing `Mcu` owns only when nobody gives it one.** The chip can be handed a clock from outside (`RP2040(clock)`, a `Simulator` built around one), and
// that clock may already hold time and alarms, so it cannot be adopted into the chip; it is used where it is (`use_clock()`), and the host that replaces it points the chip at the new
// one. Built with no clock - a wasm host, a C++ test - the chip runs on the one it owns.
//
// **The run flags are the chip's.** `request_stop()` (any thread) and the "a device waits on the real world" byte are bytes of this object, read by the loop on every iteration, so
// a host that wants the loop stopped or paced writes the chip, not a buffer of its own that the chip has to be told about.
//
// `run()` is the batch loop of `batch.hpp` with a result: why it ended, how many iterations it did, and how much simulated time it covered - the three things a host that
// paces the core against a wall clock needs and `Simulator` does not read. Simulated time is the core's own; the host sleeps for what `ticks_advanced` says.
//
// Header-only, C++17, no exceptions/RTTI/STL, no allocation. Single-threaded by contract, except that the stop byte may be written from another thread. Not movable or copyable:
// the CPU holds the address of the bus, the loop's host holds the address of the flags, and blocks will hold the address of the chip.
#ifndef RP2040PY_CORE_MCU_HPP
#define RP2040PY_CORE_MCU_HPP

#include <cstdint>

#include "batch.hpp"
#include "bus.hpp"
#include "clock.hpp"
#include "cpu.hpp"
#include "pin.hpp"
#include "pio.hpp"

namespace rp2040core {

// The limits of one `run()`: the batch's parameters (the cycle period, the instruction ceiling, the real-time budget, ...). Named for what a host calls it.
using McuLimits = BatchParams;
using McuResult = BatchResult;

class Mcu {
public:
    static constexpr int kPioBlocks = 2;

    Mcu() noexcept {
        host_.stopped = &stop_;
        host_.real_io = &real_io_;
        host_.pio_count = kPioBlocks;
        for (int i = 0; i < kPioBlocks; ++i) host_.pio_stopped[i] = &pio_[i].stopped;
        host_.pio_advance = step_pio;
        host_.monotonic = no_time;
        host_.ctx = this;
    }
    Mcu(const Mcu&) = delete;
    Mcu& operator=(const Mcu&) = delete;

    // The parts the chip owns. A host fills the bus's memory map and window table and calls `cpu.init(&bus, host, max_irq)` itself until the host interface does it.
    Cpu cpu;
    Bus bus;

    // The clock the chip runs on: its own, or the one the host gave it. Re-point whenever the host replaces it (a stale pointer is the only way to misuse this class).
    Clock& clock() noexcept { return *clock_; }
    void use_clock(Clock* clock) noexcept { clock_ = clock != nullptr ? clock : &own_clock_; }
    bool owns_clock() const noexcept { return clock_ == &own_clock_; }

    // The interrupt lines. Every block that raises one calls this (directly, through `mcu_irq_line` below, once its shell has bound it), and it reaches the CPU - unless the host has
    // put a hook over it (a test that spies on the lines, a host that wants to see them): the hook then gets the line *instead of* the CPU, as a replaced `set_interrupt` always did.
    // False: waking the core took an exception and its bus access failed (the host has parked its error).
    using IrqHook = bool (*)(void* ctx, uint32_t irq, bool level);
    bool set_interrupt(uint32_t irq, bool level) noexcept {
        return irq_hook_ != nullptr ? irq_hook_(irq_hook_ctx_, irq, level) : cpu.set_interrupt(irq, level);
    }
    void set_irq_hook(IrqHook hook, void* ctx) noexcept {
        irq_hook_ = hook;
        irq_hook_ctx_ = ctx;
    }

    // The run flags. `request_stop()` may be called from any thread; the loop leaves at its next iteration and `run()` then reports `kStopRequested`. `real_io` is nonzero while
    // some device waits on the *real* world (a relayed DNS reply, a TCP connect in flight): an idle jump is then capped and ends the batch, so simulated time cannot outrun the
    // wall clock while the reply is on its way. The addresses are for a host whose own object is a view of the byte (the Cython shell hands them to Python as buffers).
    void request_stop(bool stop = true) noexcept { stop_ = stop ? 1 : 0; }
    bool stop_requested() const noexcept { return stop_ != 0; }
    void set_real_io(bool waiting) noexcept { real_io_ = waiting ? 1 : 0; }
    bool real_io() const noexcept { return real_io_ != 0; }
    uint8_t* stop_address() noexcept { return const_cast<uint8_t*>(&stop_); }
    uint8_t* real_io_address() noexcept { return const_cast<uint8_t*>(&real_io_); }

    // The PIO blocks. The host initialises one (`init(host, first_irq, index)`) when it builds the block's shell; the loop steps whichever is running (`stopped == 0`).
    PioBlock* pio_block(int index) noexcept { return index >= 0 && index < kPioBlocks ? &pio_[index] : nullptr; }

    // What the loop reads between instructions besides what the chip owns: the "clk_sys changed" byte of the clock tree (null: the cycle period is fixed for the batch) and a
    // monotonic clock, in seconds, for the real-time budget (the default never runs the budget out). Both are the host's: the tree's byte is the tree's, and wall time is the
    // host's to give. The function receives the chip as its context.
    void set_clk_changed_flag(const volatile uint8_t* flag) noexcept { host_.clk_changed = flag; }
    void set_monotonic(double (*monotonic)(void* chip)) noexcept { host_.monotonic = monotonic != nullptr ? monotonic : no_time; }

    // Runs until the limits or a request end it. Returns 0, or `kBatchFault` when a clock alarm, the CPU or a host call failed - the host has parked its own error, and
    // `result->reason` is `kStopFault`.
    int run(const McuLimits& limits, McuResult* result = nullptr) noexcept { return run_batch(cpu, *clock_, host_, limits, result); }

private:
    static bool step_pio(void* chip, int index, int64_t cycles) noexcept { return static_cast<Mcu*>(chip)->pio_[index].advance(cycles); }
    static double no_time(void*) noexcept { return 0.0; }

    PioBlock pio_[kPioBlocks];
    IrqHook irq_hook_ = nullptr;
    void* irq_hook_ctx_ = nullptr;
    volatile uint8_t stop_ = 0;
    volatile uint8_t real_io_ = 0;
    BatchHost host_;
    Clock own_clock_;
    Clock* clock_ = &own_clock_;
};

// A block's interrupt line, bound to the chip: the context a shell hands the block as its `irq_ctx`, and the function that goes with it. The block's `irq(ctx, level)` signature is
// the common one (UART, SPI, I2C, ADC, PWM, RTC, USB, SSI: a line the shell knows); blocks with several lines use `McuIrqLines` (`irq(ctx, line, level)`, the line offset from `base`).
struct McuIrqLine {
    Mcu* mcu = nullptr;
    uint32_t line = 0;
};
inline bool mcu_irq_line(void* ctx, bool level) noexcept {
    const McuIrqLine* l = static_cast<const McuIrqLine*>(ctx);
    return l->mcu->set_interrupt(l->line, level);
}
struct McuIrqLines {
    Mcu* mcu = nullptr;
    uint32_t base = 0;
};
inline bool mcu_irq_lines(void* ctx, uint32_t line, bool level) noexcept {
    const McuIrqLines* l = static_cast<const McuIrqLines*>(ctx);
    return l->mcu->set_interrupt(l->base + line, level);
}

// The IO_BANK0 interrupt line: the level of it is "any pin's IRQ value is set", re-evaluated on the pin bank whenever a pin says its own value may have changed.
struct McuIoIrq {
    Mcu* mcu = nullptr;
    const PinBank* bank = nullptr;
    uint32_t line = 0;
};
inline bool mcu_io_interrupt(void* ctx) noexcept {
    const McuIoIrq* l = static_cast<const McuIoIrq*>(ctx);
    return l->mcu->set_interrupt(l->line, l->bank->any_irq_value());
}

}  // namespace rp2040core

#endif  // RP2040PY_CORE_MCU_HPP
