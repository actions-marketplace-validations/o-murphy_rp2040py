# Cython view of core/mcu.hpp (docs/records/0096-cpp-mcu-core.md, step 2a): the chip as one object. Header-only C++17, compiled into every extension that cimports it.

from libc.stdint cimport int64_t, uint8_t

from libcpp cimport bool

from rp2040py.native._batch cimport BatchParams, BatchResult
from rp2040py.native._bus cimport Bus
from rp2040py.native._clock cimport Clock
from rp2040py.native._cpu cimport Cpu
from rp2040py.native._pio cimport PioBlock

cdef extern from "mcu.hpp" namespace "rp2040core":
    ctypedef BatchParams McuLimits
    ctypedef BatchResult McuResult
    ctypedef double (*McuMonotonicFn)(void* chip) noexcept

    cdef cppclass Mcu:
        Mcu() noexcept
        Cpu cpu
        Bus bus
        Clock& clock() noexcept
        void use_clock(Clock* clock) noexcept
        bool owns_clock() noexcept
        void request_stop(bool stop) noexcept
        bool stop_requested() noexcept
        void set_real_io(bool waiting) noexcept
        bool real_io() noexcept
        uint8_t* stop_address() noexcept
        uint8_t* real_io_address() noexcept
        PioBlock* pio_block(int index) noexcept
        void set_clk_changed_flag(const uint8_t* flag) noexcept
        void set_monotonic(McuMonotonicFn monotonic) noexcept
        int run(const McuLimits& limits, McuResult* result) noexcept
