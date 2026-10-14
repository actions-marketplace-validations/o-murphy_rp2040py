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
#include "dma.hpp"
#include "pin.hpp"
#include "pio.hpp"
#include "busctrl.hpp"
#include "rtc.hpp"
#include "syscfg.hpp"
#include "sysinfo.hpp"
#include "tbman.hpp"
#include "vreg.hpp"

namespace rp2040core {

// The limits of one `run()`: the batch's parameters (the cycle period, the instruction ceiling, the real-time budget, ...). Named for what a host calls it.
using McuLimits = BatchParams;
using McuResult = BatchResult;

// What the world supplies to a chip that builds itself (step 3a of the revised plan, record 0096): the one interface the blocks go through for what the core cannot answer alone.
// For now it is the warning (a register access the block does not implement: the logger of the host); the pins, the console and the rest come with the blocks that need them.
// `warn(ctx, block, kind, offset, value)`: `block` is a `McuBlock`, `kind` the `kRegWarn*` code of core_host.hpp. `failed` is the host's parked-failure flag, shared by every block.
enum McuBlock : uint32_t { kMcuBlockRtc = 0, kMcuBlockTbman = 1, kMcuBlockBusctrl = 2, kMcuBlockVreg = 3, kMcuBlockSysinfo = 4, kMcuBlockSyscfg = 5 };
struct McuHost {
    void (*warn)(void* ctx, uint32_t block, uint32_t kind, uint32_t offset, int64_t value) = nullptr;
    void* ctx = nullptr;
    const int* failed = nullptr;
};

constexpr uint32_t kRtcBase = 0x4005C000;    // RTC_BASE
constexpr uint32_t kTbmanBase = 0x4006C000;  // TBMAN_BASE
constexpr uint32_t kBusctrlBase = 0x40030000;  // BUSCTRL_BASE
constexpr uint32_t kVregBase = 0x40064000;     // VREG_AND_CHIP_RESET_BASE
constexpr uint32_t kSysinfoBase = 0x40000000;  // SYSINFO_BASE
constexpr uint32_t kSyscfgBase = 0x40004000;   // SYSCFG_BASE
constexpr uint32_t kRomVersionAddress = 0x13;  // the bootrom's version byte (pico-sdk rp2040_rom_version(): the ROM header at 0x10, the fourth byte)
constexpr uint32_t kIrqRtc = 25;           // RTC_IRQ

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
    ~Mcu() { rtc_.detach(); }  // (unlinks the second from the clock, which may be gone already: safe)

    // Builds the blocks that live in this class: initialises each on the chip's clock, maps its window on the bus and binds its interrupt line, all here and not in a shell. Call once,
    // after `use_clock()` (the RTC's second is an alarm of that clock) and with the host that answers for what the chip cannot. The blocks come over one at a time (record 0096, 3a): the RTC first.
    void build(const McuHost& host) noexcept {
        host_world_ = host;
        RtcHost h;
        h.warn = block_warn<kMcuBlockRtc>;
        h.irq = rtc_irq;
        h.ctx = this;
        h.failed = host.failed;
        rtc_.init(clock_, h);
        bus.windows.attach(kRtcBase, rtc_.window_handler());

        TbmanHost t;
        t.warn = block_warn<kMcuBlockTbman>;
        t.ctx = this;
        t.failed = host.failed;
        tbman_.init(t);
        bus.windows.attach(kTbmanBase, tbman_.window_handler());

        BusctrlHost b;
        b.warn = block_warn<kMcuBlockBusctrl>;
        b.ctx = this;
        b.failed = host.failed;
        busctrl_.init(b);
        bus.windows.attach(kBusctrlBase, busctrl_.window_handler());

        VregHost v;
        v.warn = block_warn<kMcuBlockVreg>;
        v.ctx = this;
        v.failed = host.failed;
        vreg_.init(v);
        bus.windows.attach(kVregBase, vreg_.window_handler());

        SysInfoHost i;
        i.rom_version = sysinfo_rom_version;  // read off the chip's own bus: the bootrom the chip holds
        i.warn = block_warn<kMcuBlockSysinfo>;
        i.ctx = this;
        i.failed = host.failed;
        sysinfo_.init(i);
        bus.windows.attach(kSysinfoBase, sysinfo_.window_handler());

        SyscfgHost c;
        c.warn = block_warn<kMcuBlockSyscfg>;
        c.ctx = this;
        c.failed = host.failed;
        syscfg_.init(&cpu, c);  // (PROC0_NMI_MASK is the chip's own CPU)
        bus.windows.attach(kSyscfgBase, syscfg_.window_handler());
    }
    RtcBlock& rtc() noexcept { return rtc_; }
    TbmanBlock& tbman() noexcept { return tbman_; }
    BusctrlBlock& busctrl() noexcept { return busctrl_; }
    VregBlock& vreg() noexcept { return vreg_; }
    SysInfoBlock& sysinfo() noexcept { return sysinfo_; }
    SyscfgBlock& syscfg() noexcept { return syscfg_; }

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

    // The DMA requests (step 2c). Every block with a DREQ calls this (directly, through the adapters below, once its shell has bound it) and it reaches the DMA block the host
    // attached - unless a hook is over it (a test double that records the requests, a DMA that is not the native one): the hook then gets the request *instead of* the DMA, as a
    // replaced `rp2040.dma` always did. With neither, a request goes nowhere (a chip without a DMA). False: waking a channel failed (the host has parked its error).
    // The attached block must outlive its attachment: the host re-attaches (or detaches, with null) whenever it replaces the DMA. Until the DMA is a member of this class (3a) it is the host's.
    using DreqHook = bool (*)(void* ctx, uint32_t number, bool asserted);
    bool set_dreq(uint32_t number, bool asserted) noexcept {
        if (dreq_hook_ != nullptr) return dreq_hook_(dreq_hook_ctx_, number, asserted);
        if (dma_ == nullptr) return true;
        if (asserted) return dma_->set_dreq(number);
        dma_->clear_dreq(number);
        return true;
    }
    void attach_dma(DmaBlock* dma) noexcept { dma_ = dma; }
    void set_dreq_hook(DreqHook hook, void* ctx) noexcept {
        dreq_hook_ = hook;
        dreq_hook_ctx_ = ctx;
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
    // A block's `warn(ctx, kind, offset, value)` onto the host's `warn(ctx, block, kind, offset, value)`: the block id is the template's, so every block shares this one function.
    template <McuBlock Block>
    static void block_warn(void* chip, uint32_t kind, uint32_t offset, int64_t value) noexcept {
        Mcu* mcu = static_cast<Mcu*>(chip);
        if (mcu->host_world_.warn != nullptr) mcu->host_world_.warn(mcu->host_world_.ctx, Block, kind, offset, value);
    }
    static bool sysinfo_rom_version(void* chip, uint32_t* version) noexcept {
        *version = static_cast<Mcu*>(chip)->bus.read8(kRomVersionAddress);
        return true;
    }
    static bool rtc_irq(void* chip, bool level) noexcept { return static_cast<Mcu*>(chip)->set_interrupt(kIrqRtc, level); }

    PioBlock pio_[kPioBlocks];
    RtcBlock rtc_;
    TbmanBlock tbman_;
    BusctrlBlock busctrl_;
    VregBlock vreg_;
    SysInfoBlock sysinfo_;
    SyscfgBlock syscfg_;
    McuHost host_world_;
    IrqHook irq_hook_ = nullptr;
    void* irq_hook_ctx_ = nullptr;
    DmaBlock* dma_ = nullptr;
    DreqHook dreq_hook_ = nullptr;
    void* dreq_hook_ctx_ = nullptr;
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

// A block's DMA request, bound to the chip: the context a shell hands the block as its `dreq_ctx`, and the function that goes with it, by the shape of the block's own `dreq`.
// `McuDreqLine`: one request, `dreq(ctx, asserted)` (ADC, XIP stream). `McuDreqPair`: a TX and an RX request, `dreq(ctx, which, asserted)` - `if_true` is the request `which == true`
// selects (the UART passes `rx`, the SPI and SSI `tx`, so each shell says which is which). `McuDreqBase`: the PWM's `dreq(ctx, channel)`, which only ever asserts (the pulse of a
// wrap), the request being `base + channel`. The PIO needs no adapter state: its `dreq(ctx, number, asserted)` already carries the absolute number, so its context is the `Mcu`.
struct McuDreqLine {
    Mcu* mcu = nullptr;
    uint32_t number = 0;
};
inline bool mcu_dreq_line(void* ctx, bool asserted) noexcept {
    const McuDreqLine* l = static_cast<const McuDreqLine*>(ctx);
    return l->mcu->set_dreq(l->number, asserted);
}
struct McuDreqPair {
    Mcu* mcu = nullptr;
    uint32_t if_true = 0, if_false = 0;
};
inline bool mcu_dreq_pair(void* ctx, bool which, bool asserted) noexcept {
    const McuDreqPair* l = static_cast<const McuDreqPair*>(ctx);
    return l->mcu->set_dreq(which ? l->if_true : l->if_false, asserted);
}
struct McuDreqBase {
    Mcu* mcu = nullptr;
    uint32_t base = 0;
};
inline bool mcu_dreq_base(void* ctx, uint32_t channel) noexcept {
    const McuDreqBase* l = static_cast<const McuDreqBase*>(ctx);
    return l->mcu->set_dreq(l->base + channel, true);
}
inline bool mcu_dreq_number(void* ctx, uint32_t number, bool asserted) noexcept { return static_cast<Mcu*>(ctx)->set_dreq(number, asserted); }

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
