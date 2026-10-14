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

#include "adc.hpp"
#include "batch.hpp"
#include "bus.hpp"
#include "clock.hpp"
#include "cpu.hpp"
#include "dma.hpp"
#include "i2c.hpp"
#include "pll.hpp"
#include "pin.hpp"
#include "pio.hpp"
#include "busctrl.hpp"
#include "clock_sinks.hpp"
#include "clocks.hpp"
#include "rosc.hpp"
#include "rtc.hpp"
#include "spi.hpp"
#include "syscfg.hpp"
#include "sysinfo.hpp"
#include "tbman.hpp"
#include "timer.hpp"
#include "uart.hpp"
#include "vreg.hpp"
#include "xosc.hpp"

namespace rp2040core {

// The limits of one `run()`: the batch's parameters (the cycle period, the instruction ceiling, the real-time budget, ...). Named for what a host calls it.
using McuLimits = BatchParams;
using McuResult = BatchResult;

// What the world supplies to a chip that builds itself (step 3a of the revised plan, record 0096): the one interface the blocks go through for what the core cannot answer alone.
// For now it is the warning (a register access the block does not implement: the logger of the host); the pins, the console and the rest come with the blocks that need them.
// `warn(ctx, block, kind, offset, value)`: `block` is a `McuBlock`, `kind` the `kRegWarn*` code of core_host.hpp. `failed` is the host's parked-failure flag, shared by every block.
enum McuBlock : uint32_t { kMcuBlockRtc = 0, kMcuBlockTbman = 1, kMcuBlockBusctrl = 2, kMcuBlockVreg = 3, kMcuBlockSysinfo = 4, kMcuBlockSyscfg = 5, kMcuBlockTimer = 6, kMcuBlockAdc = 7, kMcuBlockUart0 = 8, kMcuBlockUart1 = 9, kMcuBlockSpi0 = 10, kMcuBlockSpi1 = 11, kMcuBlockI2c0 = 12, kMcuBlockI2c1 = 13, kMcuBlockClocks = 14, kMcuBlockPllSys = 15, kMcuBlockPllUsb = 16, kMcuBlockXosc = 17, kMcuBlockRosc = 18 };
struct McuHost {
    void (*warn)(void* ctx, uint32_t block, uint32_t kind, uint32_t offset, int64_t value) = nullptr;
    void* ctx = nullptr;
    const int* failed = nullptr;

    // The devices the world attaches to the peripherals, and the oscillators a host may have swapped for its own (`xosc_hz` / `rosc_hz`: the output in Hz, 0 while not running). Every one is optional: with none, a bus has no device on it (an I2C that NACKs every
    // address, an SPI that reads back 0, an ADC that converts what the default device gives), a UART's bytes go nowhere. `instance` is the UART / SPI / I2C number.
    // False from any of them: the host has parked its error (as everywhere), and the block's access fails with it.
    bool (*uart_byte)(void* ctx, uint32_t instance, uint32_t byte) = nullptr;           // a transmitted byte
    bool (*uart_baud)(void* ctx, uint32_t instance) = nullptr;                          // IBRD/FBRD changed: the host reads the new baud rate off the block
    bool (*spi_transmit)(void* ctx, uint32_t instance, uint32_t value) = nullptr;       // a word is on the wire; the device answers with the block's `complete_transmit()`
    bool (*i2c_start)(void* ctx, uint32_t instance, bool repeated) = nullptr;           // the device is asked to produce a (repeated) START ...
    bool (*i2c_connect)(void* ctx, uint32_t instance, uint32_t address, uint32_t mode) = nullptr;  // ... to address a target (mode 0 = write, 1 = read) ...
    bool (*i2c_write)(void* ctx, uint32_t instance, uint32_t value) = nullptr;          // ... to take a byte ...
    bool (*i2c_read)(void* ctx, uint32_t instance, bool ack) = nullptr;                 // ... to give a byte ...
    bool (*i2c_stop)(void* ctx, uint32_t instance) = nullptr;                           // ... to produce a STOP; each answers with the block's `complete_*()`
    double (*xosc_hz)(void* ctx) = nullptr;                                             // only while the host has put its own crystal block over the chip's
    double (*rosc_hz)(void* ctx) = nullptr;                                             // ... or its own ring oscillator
    bool (*adc_read)(void* ctx, uint32_t channel) = nullptr;                            // a conversion was requested; the device answers by pushing a sample
    bool (*adc_channel_value)(void* ctx, uint32_t channel, int64_t* value) = nullptr;   // the analog value of a channel
};

constexpr uint32_t kRtcBase = 0x4005C000;    // RTC_BASE
constexpr uint32_t kClocksBase = 0x40008000, kPllSysBase = 0x40028000, kPllUsbBase = 0x4002C000, kXoscBase = 0x40024000, kRoscBase = 0x40060000;
constexpr uint32_t kTimerBase = 0x40054000;  // TIMER_BASE (the lines are 0..3)
constexpr uint32_t kAdcBase = 0x4004C000;    // ADC_BASE
constexpr uint32_t kIrqAdc = 22;             // ADC_IRQ_FIFO
constexpr uint32_t kDreqAdc = 36;            // DREQ_ADC
constexpr uint32_t kUartBase[2] = {0x40034000, 0x40038000}, kIrqUart[2] = {20, 21}, kDreqUartTx[2] = {20, 22};  // (the RX request is the TX one + 1)
constexpr uint32_t kSpiBase[2] = {0x4003C000, 0x40040000}, kIrqSpi[2] = {18, 19}, kDreqSpiTx[2] = {16, 18};
constexpr uint32_t kI2cBase[2] = {0x40044000, 0x40048000}, kIrqI2c[2] = {23, 24};
constexpr uint32_t kTbmanBase = 0x4006C000;  // TBMAN_BASE
constexpr uint32_t kBusctrlBase = 0x40030000;  // BUSCTRL_BASE
constexpr uint32_t kVregBase = 0x40064000;     // VREG_AND_CHIP_RESET_BASE
constexpr uint32_t kSysinfoBase = 0x40000000;  // SYSINFO_BASE
constexpr uint32_t kSyscfgBase = 0x40004000;   // SYSCFG_BASE
constexpr uint32_t kRomVersionAddress = 0x13;  // the bootrom's version byte (pico-sdk rp2040_rom_version(): the ROM header at 0x10, the fourth byte)
constexpr uint32_t kIrqRtc = 25;           // RTC_IRQ

class Mcu;
struct McuInstance {  // the context of one UART / SPI / I2C: the chip and which of the two it is
    Mcu* mcu = nullptr;
    uint32_t index = 0;
};

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
    ~Mcu() {  // (unlinks every alarm from the clock, which may be gone already: safe)
        rtc_.detach();
        clocks_.detach();
        rosc_.detach();
        timer_.detach();
        adc_.detach();
        for (int i = 0; i < 2; ++i) {
            uart_[i].detach();
            spi_[i].detach();
        }
    }

    // Builds the blocks that live in this class: initialises each on the chip's clock, maps its window on the bus and binds its interrupt line, all here and not in a shell. Call once,
    // after `use_clock()` (the RTC's second is an alarm of that clock) and with the host that answers for what the chip cannot. The blocks come over one at a time (record 0096, 3a): the RTC first.
    void build(const McuHost& host) noexcept {
        host_world_ = host;
        // The clock tree (CLOCKS, both PLLs, the crystal and the ring oscillator): built before the blocks that run from it. The PLLs take the crystal from the tree, the oscillators and the PLL
        // registers tell the tree when they change, and the tree reads both oscillators off their blocks. The crystal's frequency is the board's: `set_crystal_hz()`.
        PllHost ps;
        ps.warn = block_warn<kMcuBlockPllSys>;
        ps.changed = tree_changed;
        ps.ctx = this;
        ps.failed = host.failed;
        pll_[0].init(ps);
        PllHost pu = ps;
        pu.warn = block_warn<kMcuBlockPllUsb>;
        pll_[1].init(pu);
        XoscHost xo;
        xo.warn = block_warn<kMcuBlockXosc>;
        xo.stable_changed = tree_changed;
        xo.ctx = this;
        xo.failed = host.failed;
        xosc_.init(xo);
        RoscHost ro;
        ro.warn = block_warn<kMcuBlockRosc>;
        ro.frequency_changed = rosc_changed;  // (no random source: the block has its own)
        ro.ctx = this;
        ro.failed = host.failed;
        rosc_.init(clock_, ro);
        ClocksHost ch;
        ch.warn = block_warn<kMcuBlockClocks>;
        ch.xosc_hz = tree_xosc_hz;  // (read only while a host's own oscillator block stands in for the chip's)
        ch.rosc_hz = tree_rosc_hz;
        ch.ctx = this;
        ch.failed = host.failed;
        clocks_.init(clock_, ch, &pll_[0], &pll_[1]);
        for (int i = 0; i < 2; ++i) pll_[i].bind_reference(clocks_xosc_hz, &clocks_);
        clocks_.bind_xosc(&xosc_);
        clocks_.bind_rosc(&rosc_);
        bus.windows.attach(kClocksBase, clocks_.window_handler());
        bus.windows.attach(kPllSysBase, pll_[0].window_handler());
        bus.windows.attach(kPllUsbBase, pll_[1].window_handler());
        bus.windows.attach(kXoscBase, xosc_.window_handler());
        bus.windows.attach(kRoscBase, rosc_.window_handler());

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

        // The blocks with lines, requests and devices (group 2). Each instance's callbacks get its own `McuInstance` as their context.
        TimerHost tm;
        tm.irq = timer_irq;
        tm.warn = block_warn<kMcuBlockTimer>;
        tm.ctx = this;
        timer_.init(clock_, tm);
        bus.windows.attach(kTimerBase, timer_.window_handler());

        AdcHost ad;
        ad.irq = adc_irq;
        ad.dreq = adc_dreq;
        ad.read = adc_read;
        ad.channel_value = adc_channel_value;
        ad.warn = block_warn<kMcuBlockAdc>;
        ad.ctx = this;
        ad.failed = host.failed;
        adc_.init(clock_, ad);
        bus.windows.attach(kAdcBase, adc_.window_handler());

        for (uint32_t i = 0; i < 2; ++i) {
            instance_[i] = {this, i};
            UartHost u;
            u.irq = uart_irq;
            u.dreq = uart_dreq;
            u.on_byte = uart_byte;
            u.baud_changed = uart_baud;
            u.bit_ns = uart_bit_ns;
            u.warn = i == 0 ? instance_warn<kMcuBlockUart0> : instance_warn<kMcuBlockUart1>;
            u.ctx = &instance_[i];
            u.failed = host.failed;
            uart_[i].init(clock_, u);
            bus.windows.attach(kUartBase[i], uart_[i].window_handler());

            SpiHost sp;
            sp.irq = spi_irq;
            sp.dreq = spi_dreq;
            sp.transmit = spi_transmit;
            sp.bit_ns = spi_bit_ns;
            sp.warn = i == 0 ? instance_warn<kMcuBlockSpi0> : instance_warn<kMcuBlockSpi1>;
            sp.ctx = &instance_[i];
            sp.failed = host.failed;
            spi_[i].init(clock_, sp);
            bus.windows.attach(kSpiBase[i], spi_[i].window_handler());

            I2cHost ic;
            ic.irq = i2c_irq;
            ic.start = i2c_start;
            ic.connect = i2c_connect;
            ic.write_byte = i2c_write;
            ic.read_byte = i2c_read;
            ic.stop = i2c_stop;
            ic.warn = i == 0 ? instance_warn<kMcuBlockI2c0> : instance_warn<kMcuBlockI2c1>;
            ic.ctx = &instance_[i];
            ic.failed = host.failed;
            i2c_[i].init(ic);
            bus.windows.attach(kI2cBase[i], i2c_[i].window_handler());
        }
    }
    RtcBlock& rtc() noexcept { return rtc_; }
    TbmanBlock& tbman() noexcept { return tbman_; }
    BusctrlBlock& busctrl() noexcept { return busctrl_; }
    VregBlock& vreg() noexcept { return vreg_; }
    SysInfoBlock& sysinfo() noexcept { return sysinfo_; }
    SyscfgBlock& syscfg() noexcept { return syscfg_; }
    ClocksBlock& clocks() noexcept { return clocks_; }
    PllBlock& pll(uint32_t i) noexcept { return pll_[i & 1]; }
    XoscBlock& xosc() noexcept { return xosc_; }
    RoscBlock& rosc() noexcept { return rosc_; }
    TimerBlock& timer() noexcept { return timer_; }
    AdcBlock& adc() noexcept { return adc_; }
    UartBlock& uart(uint32_t i) noexcept { return uart_[i & 1]; }
    SpiBlock& spi(uint32_t i) noexcept { return spi_[i & 1]; }
    I2cBlock& i2c(uint32_t i) noexcept { return i2c_[i & 1]; }

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
    // The clock tree. The crystal is the board's (12 MHz on a Pico): a parameter of the world. A change of an oscillator or of a PLL re-derives the tree, unless the host has put its own tree over
    // this one (a reference swapped in by an oracle): `set_tree_hook()` then gets the notice instead.
    using TreeHook = bool (*)(void* ctx);
    void set_crystal_hz(double hz) noexcept { clocks_.set_crystal_hz(hz); }
    void set_tree_hook(TreeHook hook, void* ctx) noexcept {
        tree_hook_ = hook;
        tree_hook_ctx_ = ctx;
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
    // The same for a block whose context is a `McuInstance` (a UART, SPI or I2C).
    template <McuBlock Block>
    static void instance_warn(void* ctx, uint32_t kind, uint32_t offset, int64_t value) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        if (i->mcu->host_world_.warn != nullptr) i->mcu->host_world_.warn(i->mcu->host_world_.ctx, Block, kind, offset, value);
    }
    static bool tree_changed(void* chip) noexcept {
        Mcu* m = static_cast<Mcu*>(chip);
        return m->tree_hook_ != nullptr ? m->tree_hook_(m->tree_hook_ctx_) : m->clocks_.retune();
    }
    static double tree_xosc_hz(void* chip) noexcept {
        Mcu* m = static_cast<Mcu*>(chip);
        return m->host_world_.xosc_hz != nullptr ? m->host_world_.xosc_hz(m->host_world_.ctx) : 0.0;
    }
    static double tree_rosc_hz(void* chip) noexcept {
        Mcu* m = static_cast<Mcu*>(chip);
        return m->host_world_.rosc_hz != nullptr ? m->host_world_.rosc_hz(m->host_world_.ctx) : 0.0;
    }
    static bool rosc_changed(void* chip, double) noexcept { return tree_changed(chip); }
    static bool sysinfo_rom_version(void* chip, uint32_t* version) noexcept {
        *version = static_cast<Mcu*>(chip)->bus.read8(kRomVersionAddress);
        return true;
    }
    // The blocks of group 2 call these; each raises its line / request on the chip, or hands the world the part it answers for. `ctx` is the Mcu (TIMER, ADC) or a `McuInstance`.
    static bool timer_irq(void* chip, uint32_t line, bool level) noexcept { return static_cast<Mcu*>(chip)->set_interrupt(line, level); }
    static bool adc_irq(void* chip, bool level) noexcept { return static_cast<Mcu*>(chip)->set_interrupt(kIrqAdc, level); }
    static bool adc_dreq(void* chip, bool asserted) noexcept { return static_cast<Mcu*>(chip)->set_dreq(kDreqAdc, asserted); }
    static bool adc_read(void* chip, uint32_t channel) noexcept {
        Mcu* m = static_cast<Mcu*>(chip);
        if (m->host_world_.adc_read != nullptr) return m->host_world_.adc_read(m->host_world_.ctx, channel);
        m->adc_.default_adc_read(channel);
        return true;
    }
    static bool adc_channel_value(void* chip, uint32_t channel, int64_t* value) noexcept {
        Mcu* m = static_cast<Mcu*>(chip);
        if (m->host_world_.adc_channel_value != nullptr) return m->host_world_.adc_channel_value(m->host_world_.ctx, channel, value);
        *value = 0;
        return true;
    }
    static bool uart_irq(void* ctx, bool level) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        return i->mcu->set_interrupt(kIrqUart[i->index], level);
    }
    static bool uart_dreq(void* ctx, bool rx, bool asserted) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        return i->mcu->set_dreq(kDreqUartTx[i->index] + (rx ? 1u : 0u), asserted);
    }
    static bool uart_byte(void* ctx, uint32_t byte) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const McuHost& w = i->mcu->host_world_;
        return w.uart_byte == nullptr || w.uart_byte(w.ctx, i->index, byte);
    }
    static bool uart_baud(void* ctx) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const McuHost& w = i->mcu->host_world_;
        return w.uart_baud == nullptr || w.uart_baud(w.ctx, i->index);
    }
    static bool uart_bit_ns(void* ctx, double* nanos) noexcept {  // 16 x divisor bit-clocks of clk_peri per bit; 0 while the divisor or the clock is zero
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const UartBlock& u = i->mcu->uart_[i->index];
        const double divider = u.int_divisor + u.frac_divisor / 64.0;
        const double clk_peri = i->mcu->clocks_.clk_peri();
        *nanos = (divider <= 0 || clk_peri <= 0) ? 0.0 : 16 * divider * 1e9 / clk_peri;
        return true;
    }
    static bool spi_irq(void* ctx, bool level) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        return i->mcu->set_interrupt(kIrqSpi[i->index], level);
    }
    static bool spi_dreq(void* ctx, bool tx, bool asserted) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        return i->mcu->set_dreq(kDreqSpiTx[i->index] + (tx ? 0u : 1u), asserted);
    }
    static bool spi_transmit(void* ctx, uint32_t value) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const McuHost& w = i->mcu->host_world_;
        if (w.spi_transmit != nullptr) return w.spi_transmit(w.ctx, i->index, value);
        return i->mcu->spi_[i->index].complete_transmit(0);  // no device: the byte comes straight back as 0
    }
    static bool spi_bit_ns(void* ctx, double* nanos) noexcept {  // one SSPCLKOUT period: clk_peri / (CPSR x (1 + SCR)); 0 while the prescale divisor is 0
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const SpiBlock& sp = i->mcu->spi_[i->index];
        const uint32_t scr = (sp.control0 >> spi_regs::CR0_SCR_SHIFT) & spi_regs::CR0_SCR_MASK;
        const double frequency = sp.clock_divisor == 0 ? 0.0 : i->mcu->clocks_.clk_peri() / (static_cast<double>(sp.clock_divisor) * (1 + scr));
        *nanos = frequency != 0.0 ? 1e9 / frequency : 0.0;
        return true;
    }
    static bool i2c_irq(void* ctx, bool level) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        return i->mcu->set_interrupt(kIrqI2c[i->index], level);
    }
    static bool i2c_start(void* ctx, bool repeated) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const McuHost& w = i->mcu->host_world_;
        return w.i2c_start != nullptr ? w.i2c_start(w.ctx, i->index, repeated) : i->mcu->i2c_[i->index].complete_start();
    }
    static bool i2c_connect(void* ctx, uint32_t address, uint32_t mode) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const McuHost& w = i->mcu->host_world_;
        return w.i2c_connect != nullptr ? w.i2c_connect(w.ctx, i->index, address, mode) : i->mcu->i2c_[i->index].complete_connect(false, 0);  // no device: every address is NACKed
    }
    static bool i2c_write(void* ctx, uint32_t value) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const McuHost& w = i->mcu->host_world_;
        return w.i2c_write != nullptr ? w.i2c_write(w.ctx, i->index, value) : i->mcu->i2c_[i->index].complete_write(false);
    }
    static bool i2c_read(void* ctx, bool ack) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const McuHost& w = i->mcu->host_world_;
        return w.i2c_read != nullptr ? w.i2c_read(w.ctx, i->index, ack) : i->mcu->i2c_[i->index].complete_read(0xFF);
    }
    static bool i2c_stop(void* ctx) noexcept {
        const McuInstance* i = static_cast<const McuInstance*>(ctx);
        const McuHost& w = i->mcu->host_world_;
        return w.i2c_stop != nullptr ? w.i2c_stop(w.ctx, i->index) : i->mcu->i2c_[i->index].complete_stop();
    }
    static bool rtc_irq(void* chip, bool level) noexcept { return static_cast<Mcu*>(chip)->set_interrupt(kIrqRtc, level); }

    PioBlock pio_[kPioBlocks];
    ClocksBlock clocks_;
    PllBlock pll_[2];
    XoscBlock xosc_;
    RoscBlock rosc_;
    TreeHook tree_hook_ = nullptr;
    void* tree_hook_ctx_ = nullptr;
    RtcBlock rtc_;
    TbmanBlock tbman_;
    BusctrlBlock busctrl_;
    VregBlock vreg_;
    SysInfoBlock sysinfo_;
    SyscfgBlock syscfg_;
    TimerBlock timer_;
    AdcBlock adc_;
    UartBlock uart_[2];
    SpiBlock spi_[2];
    I2cBlock i2c_[2];
    McuInstance instance_[2];
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
