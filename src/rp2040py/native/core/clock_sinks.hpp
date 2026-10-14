// The consumers of the clock tree that live in C++, as sinks for `ClocksBlock` (core/clocks.hpp; docs/tasks/clocks-port.md): the tree tells a block of a change through a function pointer and a
// context, and these are the four adapters from that shape to the blocks' own methods - SysTick (the PPB follows `clk_sys`), the PWM counters (they count `clk_sys`), the watchdog's tick (it derives from
// `clk_ref`) and the RTC (it counts `clk_rtc`). The context is the block. What is not here is not a block's business: the chip's own `clk_sys` / `clk_peri` values, the host's listeners and the UARTs' baud rate
// announcement all end in the host, and the host registers its own sinks for them.
//
// Header-only, C++17, no exceptions/RTTI/STL, no allocation.
#ifndef RP2040PY_CORE_CLOCK_SINKS_HPP
#define RP2040PY_CORE_CLOCK_SINKS_HPP

#include "ppb.hpp"
#include "pwm.hpp"
#include "rtc.hpp"
#include "watchdog.hpp"

namespace rp2040core {

inline bool ppb_sys_sink(void* ctx, double hz, double) noexcept {
    static_cast<PpbBlock*>(ctx)->clk_sys_changed(hz);
    return true;
}

inline bool pwm_sys_sink(void* ctx, double hz, double) noexcept {
    static_cast<PwmBlock*>(ctx)->set_clock_freq(hz);
    return true;
}

inline bool watchdog_ref_sink(void* ctx, double hz, double) noexcept { return static_cast<WatchdogBlock*>(ctx)->clk_ref_changed(hz); }

inline bool rtc_rtc_sink(void* ctx, double hz, double) noexcept { return static_cast<RtcBlock*>(ctx)->clk_rtc_changed(hz); }

}  // namespace rp2040core

#endif  // RP2040PY_CORE_CLOCK_SINKS_HPP
