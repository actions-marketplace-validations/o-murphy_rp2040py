// Standalone checks of src/rp2040py/native/core/usb.hpp (see tests/test_core_cpp.py for the flags).
//
// The end-to-end proof is tests/test_usb_diff.py (the reference against the chip's USB, thousands of random steps, every logic mutant of the reference caught); these are the directed checks
// that need no Python: the power-on registers and masks, an IN and an OUT transfer in device mode (status write-back, BUFF_STATUS, TRANS_COMPLETE, the interrupt line), FULL / STALL gating,
// double buffering and the isochronous offset, the BUFF_STATUS FIFO and BUFF_CPU_SHOULD_HANDLE, the write-1-to-clear rules, the bounded lists, a host-mode setup / IN / OUT sequence with a scripted
// device, SOF, and a failure injected into every host function.
#include <cstdio>
#include <cstring>

#include "usb.hpp"

using namespace rp2040core;
using namespace rp2040core::usb_regs;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static constexpr int kQueue = 8;
struct Queued {
    uint8_t data[1100];
    uint32_t size = 0, half = 0;
};
struct Env {
    int failed = 0;
    int fail_at = -1;  // fail the Nth host call (any function), counting from 0
    int calls = 0;
    int irq_calls = 0;
    bool irq_level = false;
    int enabled = 0, reset_received = 0, warns = 0;
    uint32_t warn_kind = 0, warn_offset = 0;
    uint32_t read_requests = 0, last_read_endpoint = 99, last_read_length = 0;
    uint32_t written = 0, last_write_endpoint = 99;
    uint8_t last_write[1100];
    uint32_t last_write_size = 0;
    Queued writes[16][kQueue];
    uint32_t write_n[16] = {};
    uint8_t reads[16][kQueue][1100];
    uint32_t read_size[16][kQueue];
    uint32_t read_n[16] = {};
    // the scripted device
    uint32_t address = 0;
    int device_resets = 0, setups = 0, ins = 0, outs = 0;
    bool setup_has_response = false;
    uint8_t pending[64];
    uint32_t pending_size = 0;
    bool pending_present = false;
    uint32_t in_status = kUsbAck;
    uint8_t in_data[64];
    uint32_t in_size = 0;
    uint32_t out_status = kUsbAck;
    uint8_t last_out[64];
    uint32_t last_out_size = 0;
};
static Env env;
static Clock clk;
static UsbBlock usb;
static uint8_t dpram[kDpramSize];

static bool call() {
    if (env.calls++ == env.fail_at) {
        env.failed = 1;
        return false;
    }
    return true;
}
static bool h_irq(void*, bool level) {
    ++env.irq_calls;
    env.irq_level = level;
    return call();
}
static bool h_enabled(void*) { ++env.enabled; return call(); }
static bool h_reset_received(void*) { ++env.reset_received; return call(); }
static bool h_endpoint_read(void*, uint32_t endpoint, uint32_t length) {
    ++env.read_requests;
    env.last_read_endpoint = endpoint;
    env.last_read_length = length;
    return call();
}
static bool h_write_queued(void*, uint32_t endpoint, uint32_t half, const uint8_t* data, uint32_t length) {
    Queued& q = env.writes[endpoint][env.write_n[endpoint]++];
    std::memcpy(q.data, data, length);
    q.size = length;
    q.half = half;
    return call();
}
static bool h_endpoint_write(void*, uint32_t endpoint, const uint8_t* data, uint32_t length) {
    ++env.written;
    env.last_write_endpoint = endpoint;
    std::memcpy(env.last_write, data, length);
    env.last_write_size = length;
    return call();
}
static bool h_drain_write(void*, uint32_t endpoint) {
    const uint32_t n = env.write_n[endpoint];
    env.write_n[endpoint] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!usb.finish_write(endpoint, env.writes[endpoint][i].data, env.writes[endpoint][i].size, env.writes[endpoint][i].half)) return false;
    }
    return true;
}
static bool h_drain_read(void*, uint32_t endpoint) {
    const uint32_t n = env.read_n[endpoint];
    env.read_n[endpoint] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!usb.finish_read(endpoint, env.reads[endpoint][i], env.read_size[endpoint][i])) return false;
    }
    return true;
}
static bool h_device_reset(void*) { ++env.device_resets; return call(); }
static bool h_device_address(void*, uint32_t* address) { *address = env.address; return call(); }
static bool h_device_setup(void*, const uint8_t*, bool* has_response) {
    ++env.setups;
    *has_response = env.setup_has_response;
    return call();
}
static bool h_pending(void*, uint32_t max_length, bool* present, const uint8_t** data, uint32_t* length) {
    *present = env.pending_present;
    if (env.pending_present) {
        *data = env.pending;
        *length = env.pending_size < max_length ? env.pending_size : max_length;
        env.pending_present = false;
    }
    return call();
}
static bool h_device_in(void*, uint32_t, uint32_t* status, const uint8_t** data, uint32_t* length) {
    ++env.ins;
    *status = env.in_status;
    *data = env.in_data;
    *length = env.in_size;
    return call();
}
static bool h_device_out(void*, uint32_t, const uint8_t* data, uint32_t length, uint32_t* status) {
    ++env.outs;
    std::memcpy(env.last_out, data, length);
    env.last_out_size = length;
    *status = env.out_status;
    return call();
}
static void h_warn(void*, uint32_t kind, uint32_t offset, int64_t) {
    ++env.warns;
    env.warn_kind = kind;
    env.warn_offset = offset;
}

static void fresh() {
    usb.detach();
    env = Env();
    clk.reset();
    std::memset(dpram, 0, sizeof dpram);
    UsbHost host;
    host.irq = h_irq;
    host.usb_enabled = h_enabled;
    host.reset_received = h_reset_received;
    host.endpoint_read = h_endpoint_read;
    host.write_queued = h_write_queued;
    host.endpoint_write = h_endpoint_write;
    host.drain_write = h_drain_write;
    host.drain_read = h_drain_read;
    host.device_reset = h_device_reset;
    host.device_address = h_device_address;
    host.device_setup = h_device_setup;
    host.pending_response = h_pending;
    host.device_in = h_device_in;
    host.device_out = h_device_out;
    host.warn = h_warn;
    host.failed = &env.failed;
    usb.init(&clk, dpram, host);
    CHECK(usb.reset());
    env.irq_calls = 0;
    env.calls = 0;
}
static uint32_t rd(uint32_t offset) { return usb.read(offset); }
static bool wr(uint32_t offset, int64_t value, uint32_t atomic = kAtomicNormal) { return usb.write_atomic(offset, value, atomic); }
static uint32_t w32(uint32_t offset) {
    return dpram[offset] | (dpram[offset + 1] << 8) | (dpram[offset + 2] << 16) | (static_cast<uint32_t>(dpram[offset + 3]) << 24);
}
static void put32(uint32_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) dpram[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
static void arm(uint32_t offset) { CHECK(usb.dpram_updated(offset)); }  // the firmware's write to a buffer-control word has landed
static const uint32_t EP1_IN_BUF = 0x88, EP1_OUT_BUF = 0x8C;
static void tick_us(double us) { CHECK(clk.tick(us * 1000.0)); }

static void test_power_on_state() {
    fresh();
    CHECK(rd(NAK_POLL) == NAK_POLL_RESET && rd(USBPHY_TRIM) == USBPHY_TRIM_RESET);
    CHECK(rd(MAIN_CTRL) == 0 && rd(SIE_STATUS) == 0 && rd(INTR) == 0 && rd(INTS) == 0 && rd(BUFF_STATUS) == 0 && rd(BUFF_CPU_SHOULD_HANDLE) == 0);
    CHECK(!env.irq_level);
    CHECK(rd(SOF_WR) == 0);
}

static void test_masks() {
    fresh();
    wr(ADDR_ENDP, 0xFFFFFFFF);
    CHECK(rd(ADDR_ENDP) == ADDR_ENDP_MASK);
    wr(ADDR_ENDP15, 0xFFFFFFFF);
    CHECK(rd(ADDR_ENDP15) == ADDR_ENDPN_MASK);
    wr(SIE_CTRL, 0xFFFFFFFF);
    CHECK(rd(SIE_CTRL) == (SIE_CTRL_MASK & ~SIE_CTRL_START_TRANS) || rd(SIE_CTRL) == SIE_CTRL_MASK);
    wr(MAIN_CTRL, 0xFFFFFFFF);
    CHECK(rd(MAIN_CTRL) == (MAIN_SIM_TIMING | MAIN_CONTROLLER_EN | MAIN_HOST_NDEVICE));
    wr(NAK_POLL, 0xFFFFFFFF);
    CHECK(rd(NAK_POLL) == NAK_POLL_MASK);
    wr(USB_MUXING, 0xFFFFFFFF);
    CHECK(rd(USB_MUXING) == USB_MUXING_MASK);
    wr(USBPHY_TRIM, 0xFFFFFFFF);
    CHECK(rd(USBPHY_TRIM) == USBPHY_TRIM_MASK);
    wr(EP_STALL_ARM, 0xFF);
    CHECK(rd(EP_STALL_ARM) == EP_STALL_ARM_MASK);
    wr(INT_EP_CTRL, 0xFFFFFFFF);
    CHECK(rd(INT_EP_CTRL) == INT_EP_CTRL_MASK);
    wr(SOF_WR, 0xFFFFFFFF);
    CHECK(usb.sof_frame_number == SOF_COUNT_MASK);
}

static void test_interrupt_registers_and_aliases() {
    fresh();
    CHECK(wr(INTE, 0xFFFFFFFF) && rd(INTE) == INT_MASK);
    CHECK(wr(INTF, 0x10) && rd(INTS) == 0x10 && env.irq_level);
    CHECK(wr(INTF, 0x10, kAtomicClear) && rd(INTF) == 0 && !env.irq_level);
    CHECK(wr(INTE, 0x1, kAtomicXor) && rd(INTE) == (INT_MASK ^ 1));
    CHECK(wr(INTE, 0, kAtomicSet));
    usb.int_raw = INTR_BUFF_STATUS;
    CHECK(rd(INTS) == INTR_BUFF_STATUS);
    CHECK(wr(INTE, INTR_BUFF_STATUS, kAtomicClear) && rd(INTS) == 0);
    usb.int_force = 0x1;  // forced bits are not masked by the enable
    CHECK(rd(INTS) == 0x1);
}

static void test_unimplemented_registers_warn() {
    fresh();
    CHECK(rd(0x88) == 0xFFFFFFFFu && env.warns == 1 && env.warn_kind == kUsbWarnRead && env.warn_offset == 0x88);
    CHECK(rd(0x1088) == 0xFFFFFFFFu && env.warns == 3 && env.warn_kind == kUsbWarnReadAtomicArea);
    CHECK(wr(0x9C, 1) && env.warns == 4 && env.warn_kind == kUsbWarnWrite);
}

static void test_an_in_transfer() {
    fresh();
    put32(EP1_IN_CONTROL, 0x80000000u | USB_CTRL_INTERRUPT_PER_TRANSFER | 0x200);  // enabled, interrupt per buffer, buffer at 0x200
    for (int i = 0; i < 5; ++i) dpram[0x200 + i] = static_cast<uint8_t>(0xA0 + i);
    wr(INTE, INTR_BUFF_STATUS | INTR_TRANS_COMPLETE);
    put32(EP1_IN_BUF, BUF_AVAILABLE | BUF_FULL | BUF_LAST | BUF_DATA1_PID | 5 | BUF_STALL * 0);
    arm(EP1_IN_BUF);
    CHECK(!(w32(EP1_IN_BUF) & BUF_AVAILABLE));  // the controller owns it now
    CHECK(env.write_n[1] == 1 && env.writes[1][0].size == 5 && env.writes[1][0].data[4] == 0xA4);
    CHECK(env.written == 0 && rd(BUFF_STATUS) == 0);
    tick_us(9.9);
    CHECK(env.written == 0);
    tick_us(0.2);
    CHECK(env.written == 1 && env.last_write_endpoint == 1 && env.last_write_size == 5);
    // status written back: length, pid, last - FULL is gone for an IN
    CHECK(w32(EP1_IN_BUF) == (5u | BUF_DATA1_PID | BUF_LAST));
    CHECK(rd(BUFF_STATUS) == (1u << 2));  // endpoint 1 IN is slot 2
    CHECK(rd(BUFF_CPU_SHOULD_HANDLE) == 0);
    CHECK((rd(SIE_STATUS) & (SIE_TRANS_COMPLETE | SIE_ACK_REC)) == (SIE_TRANS_COMPLETE | SIE_ACK_REC));
    CHECK(env.irq_level && (rd(INTR) & (INTR_BUFF_STATUS | INTR_TRANS_COMPLETE)) == (INTR_BUFF_STATUS | INTR_TRANS_COMPLETE));
    CHECK(wr(BUFF_STATUS, 1u << 2) && rd(BUFF_STATUS) == 0 && !(rd(INTR) & INTR_BUFF_STATUS));
    CHECK(wr(SIE_STATUS, SIE_TRANS_COMPLETE) && !(rd(INTR) & INTR_TRANS_COMPLETE) && !env.irq_level);
}

static void test_an_in_needs_full_and_a_stall_holds_it() {
    fresh();
    put32(EP1_IN_CONTROL, 0x80000000u | 0x200);
    put32(EP1_IN_BUF, BUF_AVAILABLE | 4);  // not FULL: the host would get a NAK
    arm(EP1_IN_BUF);
    CHECK(env.write_n[1] == 0 && (w32(EP1_IN_BUF) & BUF_AVAILABLE));
    put32(EP1_IN_BUF, BUF_AVAILABLE | BUF_FULL | BUF_STALL | 4);
    arm(EP1_IN_BUF);
    CHECK(env.write_n[1] == 0 && (w32(EP1_IN_BUF) & BUF_AVAILABLE));
    put32(EP1_IN_BUF, BUF_AVAILABLE | BUF_FULL | 4);
    arm(EP1_IN_BUF);
    CHECK(env.write_n[1] == 1);
}

static void test_the_interrupt_enable_decides_buff_status() {
    fresh();
    put32(EP1_IN_CONTROL, 0x80000000u | 0x200);  // no per-buffer interrupt
    put32(EP1_IN_BUF, BUF_AVAILABLE | BUF_FULL | 4);
    arm(EP1_IN_BUF);
    tick_us(11);
    CHECK(env.written == 1 && rd(BUFF_STATUS) == 0);
}

static void test_an_out_transfer() {
    fresh();
    put32(EP1_OUT_CONTROL, 0x80000000u | USB_CTRL_INTERRUPT_PER_TRANSFER | 0x300);
    put32(EP1_OUT_BUF, BUF_AVAILABLE | 8);  // room for 8
    arm(EP1_OUT_BUF);
    CHECK(env.read_requests == 1 && env.last_read_endpoint == 1 && env.last_read_length == 8);
    CHECK(usb.pending_count(1) == 1 && usb.pending_at(1, 0) == 0);
    const uint8_t host_data[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::memcpy(env.reads[1][0], host_data, 12);
    env.read_size[1][0] = 12;
    env.read_n[1] = 1;
    usb.schedule_read(1, 10);
    tick_us(10);
    CHECK(dpram[0x300] == 1 && dpram[0x307] == 8 && dpram[0x308] == 0);  // cut to the room the buffer announced
    CHECK(w32(EP1_OUT_BUF) == (8u | BUF_FULL));
    CHECK(rd(BUFF_STATUS) == (1u << 3) && usb.pending_count(1) == 0);
}

static void test_double_buffering_and_the_iso_offset() {
    fresh();
    put32(EP1_IN_CONTROL, 0x80000000u | USB_CTRL_DOUBLE_BUF | USB_CTRL_INTERRUPT_PER_TRANSFER | 0x400);
    dpram[0x400] = 0x11;
    dpram[0x440] = 0x22;
    // both halves armed
    put32(EP1_IN_BUF, (BUF_AVAILABLE | BUF_FULL | 1) | ((BUF_AVAILABLE | BUF_FULL | 1) << BUF1_SHIFT));
    arm(EP1_IN_BUF);
    CHECK(env.write_n[1] == 2 && env.writes[1][0].data[0] == 0x11 && env.writes[1][0].half == 0 && env.writes[1][1].data[0] == 0x22 && env.writes[1][1].half == 1);
    CHECK(usb.buf_select[2] == 0);  // flipped twice
    tick_us(11);
    CHECK(usb.done_count(2) == 2 && usb.done_at(2, 0) == 0 && usb.done_at(2, 1) == 1);
    CHECK(rd(BUFF_CPU_SHOULD_HANDLE) == 0);
    CHECK(wr(BUFF_STATUS, 1u << 2));
    CHECK(rd(BUFF_STATUS) == (1u << 2) && rd(BUFF_CPU_SHOULD_HANDLE) == (1u << 2));  // the bit is back, and says buffer 1
    CHECK(wr(BUFF_STATUS, 1u << 2) && rd(BUFF_STATUS) == 0);

    // an isochronous endpoint: buffer 1 sits at the offset named in bits 28:27
    fresh();
    put32(EP1_IN_CONTROL, 0x80000000u | USB_CTRL_DOUBLE_BUF | (EP_CTRL_TYPE_ISOCHRONOUS << EP_CTRL_TYPE_SHIFT) | 0x400);
    dpram[0x400 + 256] = 0x77;  // offset code 1 = 256
    put32(EP1_IN_BUF, ((BUF_AVAILABLE | BUF_FULL | 1) << BUF1_SHIFT) | (1u << BUF_ISO_OFFSET_SHIFT));
    usb.buf_select[2] = 1;
    arm(EP1_IN_BUF);
    CHECK(env.write_n[1] == 1 && env.writes[1][0].data[0] == 0x77 && env.writes[1][0].half == 1);
}

static void test_reset_select() {
    fresh();
    put32(EP1_IN_CONTROL, 0x80000000u | USB_CTRL_DOUBLE_BUF | 0x400);
    usb.buf_select[2] = 1;
    put32(EP1_IN_BUF, BUF_RESET_SELECT);
    arm(EP1_IN_BUF);
    CHECK(usb.buf_select[2] == 0);
}

static void test_ep0_uses_the_sie_ctrl_bits() {
    fresh();
    wr(SIE_CTRL, SIE_CTRL_EP0_INT_1BUF);
    put32(0x80, BUF_AVAILABLE | BUF_FULL | BUF_LAST | 3);  // EP0 IN
    arm(0x80);
    CHECK(env.write_n[0] == 1 && env.writes[0][0].size == 3);
    tick_us(11);
    CHECK(rd(BUFF_STATUS) == 1 && (rd(SIE_STATUS) & SIE_TRANS_COMPLETE));
}

static void test_setup_packet() {
    fresh();
    wr(INTE, INTR_SETUP_REQ);
    wr(EP_STALL_ARM, 3);
    const uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
    CHECK(usb.send_setup_packet(setup, 8));
    CHECK(dpram[1] == 6 && rd(EP_STALL_ARM) == 0 && (rd(SIE_STATUS) & SIE_SETUP_REC) && env.irq_level);
    CHECK(wr(SIE_STATUS, SIE_SETUP_REC) && !(rd(INTR) & INTR_SETUP_REQ) && !env.irq_level);
}

static void test_sie_status_rules() {
    fresh();
    wr(INTE, INTR_BUS_RESET | INTR_DEV_CONN_DIS);
    CHECK(usb.reset_device());
    tick_us(9999);
    CHECK(!(rd(SIE_STATUS) & SIE_BUS_RESET));
    tick_us(2);
    CHECK((rd(SIE_STATUS) & SIE_BUS_RESET) && env.irq_level);
    CHECK(wr(SIE_STATUS, SIE_BUS_RESET) && env.reset_received == 1);
    CHECK(!(rd(SIE_STATUS) & SIE_BUS_RESET) && ((rd(SIE_STATUS) >> SIE_LINE_STATE_SHIFT) & 3) == LINE_STATE_J);
    CHECK((rd(SIE_STATUS) & SIE_CONNECTED) && (rd(INTR) & INTR_DEV_CONN_DIS));  // the connect EVENT
    CHECK(wr(SIE_STATUS, SIE_CONNECTED));
    CHECK((rd(SIE_STATUS) & SIE_CONNECTED) && !(rd(INTR) & INTR_DEV_CONN_DIS));  // the level stays, the event is gone
    wr(SIE_STATUS, SIE_SPEED_MASK);
    CHECK(!usb.host_connect_event);
}

static void test_usb_pwr() {
    fresh();
    wr(USB_PWR, 0xFF);
    CHECK(rd(USB_PWR) == PWR_MASK && (rd(SIE_STATUS) & SIE_VBUS_DETECTED) && (rd(SIE_STATUS) & SIE_VBUS_OVER_CURR) && (rd(INTR) & INTR_VBUS_DETECT));
    wr(USB_PWR, PWR_VBUS_DETECT);  // override not enabled
    CHECK(!(rd(SIE_STATUS) & SIE_VBUS_DETECTED) && !(rd(SIE_STATUS) & SIE_VBUS_OVER_CURR));
}

static void test_usb_muxing_enumeration_workaround() {
    fresh();
    wr(USB_MUXING, TO_DIGITAL_PAD);
    CHECK(rd(SIE_STATUS) & SIE_CONNECTED);
    fresh();
    wr(USB_MUXING, TO_DIGITAL_PAD | TO_PHY);
    CHECK(!(rd(SIE_STATUS) & SIE_CONNECTED));
}

static void test_ep_abort_and_stall_nak() {
    fresh();
    wr(EP_ABORT, 0x6);
    CHECK(rd(EP_ABORT_DONE) == 0x6 && (rd(INTR) & INTR_ABORT_DONE));
    wr(EP_ABORT_DONE, 0x2);
    CHECK(rd(EP_ABORT_DONE) == 0x4 && (rd(INTR) & INTR_ABORT_DONE));
    wr(EP_ABORT_DONE, 0x4);
    CHECK(!(rd(INTR) & INTR_ABORT_DONE));
    usb.ep_status_stall_nak = 0x3;
    CHECK(usb.stall_nak_updated() && (rd(INTR) & INTR_EP_STALL_NAK));
    wr(EP_STATUS_STALL_NAK, 0x3);
    CHECK(!(rd(INTR) & INTR_EP_STALL_NAK));
}

static void test_bounded_lists() {
    fresh();
    for (int i = 0; i < 100; ++i) usb.done_push(5, i & 1);
    CHECK(usb.done_count(5) == kFifoDepth);
    for (uint32_t i = 0; i < kFifoDepth; ++i) CHECK(usb.done_at(5, i) == (i & 1));
    put32(EP1_OUT_CONTROL, 0x80000000u | 0x300);
    for (int i = 0; i < 100; ++i) {
        put32(EP1_OUT_BUF, BUF_AVAILABLE | 8);
        arm(EP1_OUT_BUF);
    }
    CHECK(usb.pending_count(1) == kFifoDepth && env.read_requests == 100);
}

static void test_buffers_past_the_end_of_the_dpram() {
    fresh();
    put32(EP1_IN_CONTROL, 0x80000000u | 0xFFC0);  // base 64 kB - 64: far past the 4 kB
    put32(EP1_IN_BUF, BUF_AVAILABLE | BUF_FULL | 100);
    arm(EP1_IN_BUF);
    CHECK(env.write_n[1] == 1 && env.writes[1][0].size == 0);
    put32(EP1_OUT_CONTROL, 0x80000000u | 0xF00);  // 256 bytes left, 1000 asked
    put32(EP1_OUT_BUF, BUF_AVAILABLE | 1000);
    arm(EP1_OUT_BUF);
    std::memset(env.reads[1][0], 0x5A, 1100);
    env.read_size[1][0] = 1100;
    env.read_n[1] = 1;
    usb.schedule_read(1, 10);
    tick_us(10);
    CHECK(dpram[0xFFF] == 0x5A && (w32(EP1_OUT_BUF) & BUF_LEN_MASK) == 256);
}

static void test_host_mode_control_transfer() {
    fresh();
    env.address = 0;
    wr(MAIN_CTRL, MAIN_HOST_NDEVICE | MAIN_CONTROLLER_EN);
    CHECK(usb.host_mode && env.enabled == 0);
    CHECK(usb.connect_device());
    CHECK(((rd(SIE_STATUS) >> 8) & 3) == SIE_SPEED_FS_VALUE && usb.host_connect_event && (rd(INTR) & INTR_HOST_CONN_DIS));
    // a control IN: setup, then an IN that returns the kept response cut to the buffer's length
    env.setup_has_response = true;
    for (uint32_t i = 0; i < 18; ++i) env.pending[i] = static_cast<uint8_t>(i + 1);
    env.pending_size = 18;
    env.pending_present = true;
    put32(HOST_EPX_BUF_CTRL, 8);
    wr(SIE_CTRL, SIE_CTRL_SEND_SETUP | SIE_CTRL_START_TRANS);
    CHECK(env.setups == 1 && !usb.expecting_status_phase && (rd(SIE_STATUS) & SIE_ACK_REC));
    tick_us(5);
    CHECK((rd(SIE_STATUS) & SIE_TRANS_COMPLETE) && !(rd(SIE_CTRL) & SIE_CTRL_START_TRANS));
    wr(SIE_CTRL, SIE_CTRL_RECEIVE_DATA | SIE_CTRL_START_TRANS);
    CHECK(dpram[HOST_EPX_DATA] == 1 && dpram[HOST_EPX_DATA + 7] == 8 && dpram[HOST_EPX_DATA + 8] == 0);
    CHECK(w32(HOST_EPX_BUF_CTRL) == (8u | BUF_FULL | BUF_DATA1_PID) && rd(BUFF_STATUS) == 1 && usb.control_data_pid == 0);
    tick_us(5);
    // the status phase is an OUT of zero length: acknowledged without asking the device
    env.outs = 0;
    put32(HOST_EPX_BUF_CTRL, BUF_AVAILABLE);
    wr(SIE_CTRL, SIE_CTRL_SEND_DATA | SIE_CTRL_START_TRANS);
    CHECK(env.outs == 0 && !(w32(HOST_EPX_BUF_CTRL) & BUF_AVAILABLE));
    // an OUT with data goes to the device
    tick_us(5);
    put32(HOST_EPX_BUF_CTRL, BUF_AVAILABLE | 3);
    dpram[HOST_EPX_DATA] = 9;
    wr(ADDR_ENDP, 1u << 16);  // endpoint 1
    wr(SIE_CTRL, SIE_CTRL_SEND_DATA | SIE_CTRL_START_TRANS);
    CHECK(env.outs == 1 && env.last_out_size == 3 && env.last_out[0] == 9);
    tick_us(5);
    // NAK and STALL set their status bits and move no buffer
    env.in_status = kUsbNak;
    wr(SIE_CTRL, SIE_CTRL_RECEIVE_DATA | SIE_CTRL_START_TRANS);
    CHECK(rd(SIE_STATUS) & SIE_NAK_REC);
    tick_us(5);
    env.in_status = kUsbStall;
    wr(SIE_CTRL, SIE_CTRL_RECEIVE_DATA | SIE_CTRL_START_TRANS);
    CHECK(rd(SIE_STATUS) & SIE_STALL_REC);
    // no device: a timeout
    tick_us(5);
    usb.has_device = false;
    wr(SIE_CTRL, SIE_CTRL_RECEIVE_DATA | SIE_CTRL_START_TRANS);
    CHECK(rd(SIE_STATUS) & SIE_RX_TIMEOUT);
}

static void test_host_mode_setup_without_response_expects_a_status_phase() {
    fresh();
    wr(MAIN_CTRL, MAIN_HOST_NDEVICE | MAIN_CONTROLLER_EN);
    CHECK(usb.connect_device());
    env.setup_has_response = false;
    wr(SIE_CTRL, SIE_CTRL_SEND_SETUP | SIE_CTRL_START_TRANS);
    CHECK(usb.expecting_status_phase);
    tick_us(5);
    put32(HOST_EPX_BUF_CTRL, BUF_AVAILABLE);
    wr(SIE_CTRL, SIE_CTRL_RECEIVE_DATA | SIE_CTRL_START_TRANS);
    CHECK(env.ins == 0 && !usb.expecting_status_phase);  // the zero-length ACK never asks the device
    CHECK(rd(BUFF_STATUS) == 0);
}

static void test_sof_and_interrupt_endpoints() {
    fresh();
    wr(MAIN_CTRL, MAIN_HOST_NDEVICE | MAIN_CONTROLLER_EN);
    CHECK(usb.connect_device());
    env.address = 5;
    wr(SOF_WR, 0x7FF);
    wr(SIE_CTRL, SIE_CTRL_SOF_EN);
    tick_us(999);
    CHECK(usb.sof_frame_number == 0x7FF);
    tick_us(2);
    CHECK(usb.sof_frame_number == 0 && (rd(INTR) & INTR_HOST_SOF));
    CHECK(rd(SOF_RD) == 0 && !(rd(INTR) & INTR_HOST_SOF));  // reading SOF_RD acknowledges
    // interrupt endpoint 1: device address 5, endpoint 3, IN
    wr(ADDR_ENDP1, 5u | (3u << 16));
    put32(HOST_INT_EP_CTRL_BASE, 0x500);
    put32(HOST_INT_EP_BUF_CTRL_BASE, BUF_AVAILABLE | 8);
    wr(INT_EP_CTRL, 1u << 1);
    env.in_size = 2;
    env.in_data[0] = 0xAB;
    env.in_data[1] = 0xCD;
    tick_us(1000);
    CHECK(env.ins == 1 && dpram[0x500] == 0xAB && w32(HOST_INT_EP_BUF_CTRL_BASE) == (BUF_FULL | 2u));
    CHECK(rd(BUFF_STATUS) == (1u << 2));
    tick_us(1000);
    CHECK(env.ins == 1);  // the buffer is not available again: not polled
    wr(SIE_CTRL, 0);
    tick_us(5000);
    CHECK(usb.sof_frame_number == 3);  // the SOF already pending still happens, and is not re-armed
    tick_us(5000);
    CHECK(usb.sof_frame_number == 3);
}

static void test_disconnect_and_connect_events() {
    fresh();
    wr(MAIN_CTRL, MAIN_HOST_NDEVICE | MAIN_CONTROLLER_EN);
    CHECK(usb.connect_device());
    CHECK(wr(SIE_STATUS, SIE_SPEED_MASK) && !usb.host_connect_event);
    CHECK(usb.disconnect_device());
    CHECK(((rd(SIE_STATUS) >> 8) & 3) == 0 && usb.host_connect_event && !usb.has_device);
}

static void test_reset_drops_state_and_alarms() {
    fresh();
    put32(EP1_IN_CONTROL, 0x80000000u | 0x200);
    put32(EP1_IN_BUF, BUF_AVAILABLE | BUF_FULL | 1);
    arm(EP1_IN_BUF);
    usb.reset_device();
    wr(INTE, 0x1);
    CHECK(usb.has_scheduled_alarms());
    CHECK(usb.reset());
    CHECK(!usb.has_scheduled_alarms() && rd(INTE) == 0 && rd(NAK_POLL) == NAK_POLL_RESET && !env.irq_level);
    tick_us(20000);
    CHECK(env.written == 0 && rd(SIE_STATUS) == 0);
}

static void test_failures_stop_at_once() {
    // each host call of a representative run fails in turn: the block returns false and leaves the failure flag raised
    for (int n = 0; n < 6; ++n) {
        fresh();
        env.fail_at = n;
        put32(EP1_OUT_CONTROL, 0x80000000u | USB_CTRL_INTERRUPT_PER_TRANSFER | 0x300);
        put32(EP1_OUT_BUF, BUF_AVAILABLE | 4);
        const bool armed = usb.dpram_updated(EP1_OUT_BUF);
        if (n == 0) {
            CHECK(!armed && env.failed);  // the first host call is the read request
            continue;
        }
        CHECK(armed);
        std::memset(env.reads[1][0], 1, 4);
        env.read_size[1][0] = 4;
        env.read_n[1] = 1;
        usb.schedule_read(1, 10);
        const bool ok = clk.tick(10000.0);
        if (n == 1) CHECK(!ok && env.failed);  // the interrupt line, at the first sie_status_updated
    }
    fresh();
    env.fail_at = 0;
    CHECK(!wr(INTE, 1) && env.failed);
    fresh();
    env.fail_at = 0;
    CHECK(!wr(MAIN_CTRL, MAIN_CONTROLLER_EN) || env.enabled == 1);  // on_usb_enabled is the first call
}

int main() {
    test_power_on_state();
    test_masks();
    test_interrupt_registers_and_aliases();
    test_unimplemented_registers_warn();
    test_an_in_transfer();
    test_an_in_needs_full_and_a_stall_holds_it();
    test_the_interrupt_enable_decides_buff_status();
    test_an_out_transfer();
    test_double_buffering_and_the_iso_offset();
    test_reset_select();
    test_ep0_uses_the_sie_ctrl_bits();
    test_setup_packet();
    test_sie_status_rules();
    test_usb_pwr();
    test_usb_muxing_enumeration_workaround();
    test_ep_abort_and_stall_nak();
    test_bounded_lists();
    test_buffers_past_the_end_of_the_dpram();
    test_host_mode_control_transfer();
    test_host_mode_setup_without_response_expects_a_status_phase();
    test_sof_and_interrupt_endpoints();
    test_disconnect_and_connect_events();
    test_reset_drops_state_and_alarms();
    test_failures_stop_at_once();
    if (failures == 0) std::printf("test_usb: ok\n");
    return failures == 0 ? 0 : 1;
}
