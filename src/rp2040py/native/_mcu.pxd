# Cython view of core/mcu.hpp (docs/records/0096-cpp-mcu-core.md, step 2a): the chip as one object. Header-only C++17, compiled into every extension that cimports it.

from libc.stdint cimport int64_t, uint8_t, uint32_t

from libcpp cimport bool

from rp2040py.native._batch cimport BatchParams, BatchResult
from rp2040py.native._bus cimport Bus
from rp2040py.native._clock cimport Clock
from rp2040py.native._cpu cimport Cpu
from rp2040py.native._dma cimport DmaBlock
from rp2040py.native._pio cimport PioBlock
from rp2040py.native._rtc cimport RtcBlock

cdef extern from "mcu.hpp" namespace "rp2040core":
    ctypedef BatchParams McuLimits
    ctypedef BatchResult McuResult
    ctypedef double (*McuMonotonicFn)(void* chip) noexcept
    ctypedef bool (*McuIrqHook)(void* ctx, uint32_t irq, bool level) noexcept
    ctypedef void (*McuWarnFn)(void* ctx, uint32_t block, uint32_t kind, uint32_t offset, int64_t value) noexcept
    ctypedef bool (*McuDreqHook)(void* ctx, uint32_t number, bool asserted) noexcept

    cdef const uint32_t kMcuBlockRtc

    cdef cppclass McuHost:
        McuHost() noexcept
        McuWarnFn warn
        void* ctx
        const int* failed

    cdef cppclass Mcu:
        Mcu() noexcept
        Cpu cpu
        Bus bus
        Clock& clock() noexcept
        void use_clock(Clock* clock) noexcept
        bool set_interrupt(uint32_t irq, bool level) noexcept
        void set_irq_hook(McuIrqHook hook, void* ctx) noexcept
        bool set_dreq(uint32_t number, bool asserted) noexcept
        void attach_dma(DmaBlock* dma) noexcept
        void set_dreq_hook(McuDreqHook hook, void* ctx) noexcept
        bool owns_clock() noexcept
        void request_stop(bool stop) noexcept
        bool stop_requested() noexcept
        void set_real_io(bool waiting) noexcept
        bool real_io() noexcept
        uint8_t* stop_address() noexcept
        uint8_t* real_io_address() noexcept
        PioBlock* pio_block(int index) noexcept
        void build(const McuHost& host) noexcept
        RtcBlock& rtc() noexcept
        void set_clk_changed_flag(const uint8_t* flag) noexcept
        void set_monotonic(McuMonotonicFn monotonic) noexcept
        int run(const McuLimits& limits, McuResult* result) noexcept
