// Standalone checks of the second core (docs/records/0104-second-core-seams-and-architecture.md, P1): the inter-core FIFOs, the boot ROM's launch handshake answered by the SIO, the loop for two
// cores (batch_dual.hpp) and the two ways to schedule them - the quantum with synchronisation points (F2, the main one) and the exact scheduler (quantum 1) - which must order every device
// access the same way. See tests/test_core_cpp.py for the flags.
#include <cstdio>
#include <cstring>
#include <vector>

#include "mcu.hpp"

using namespace rp2040core;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static uint8_t rom[16 * 1024], flash[64 * 1024], sram[264 * 1024], dpram[4 * 1024];
static int g_host_failed = 0;

static constexpr uint32_t kSio = 0xD0000000u;
static constexpr uint32_t FIFO_ST = 0x50, FIFO_WR = 0x54, FIFO_RD = 0x58;
static constexpr uint32_t kCode0 = 0x20000000u, kCode1 = 0x20000200u, kStack1 = 0x20001000u;

// ---- a very small Thumb assembler: just what the programs below need ---------------------------------------------------------------------------------------------------------------------------------
struct Asm {
    std::vector<uint16_t> h;
    int here() const { return static_cast<int>(h.size()); }
    void emit(uint16_t v) { h.push_back(v); }
    void movs(int rd, int imm) { emit(static_cast<uint16_t>(0x2000 | rd << 8 | imm)); }
    void adds(int rd, int imm) { emit(static_cast<uint16_t>(0x3000 | rd << 8 | imm)); }
    void ldr_pc(int rt, int word_offset) { emit(static_cast<uint16_t>(0x4800 | rt << 8 | word_offset)); }
    void ldr(int rt, int rn, int byte_offset) { emit(static_cast<uint16_t>(0x6800 | (byte_offset / 4) << 6 | rn << 3 | rt)); }
    void str(int rt, int rn, int byte_offset) { emit(static_cast<uint16_t>(0x6000 | (byte_offset / 4) << 6 | rn << 3 | rt)); }
    void tst(int rn, int rm) { emit(static_cast<uint16_t>(0x4200 | rm << 3 | rn)); }
    void beq(int to) { emit(static_cast<uint16_t>(0xD000 | ((to - (here() + 2)) & 0xFF))); }
    void b(int to) { emit(static_cast<uint16_t>(0xE000 | ((to - (here() + 2)) & 0x7FF))); }
    void nop() { emit(0xBF00); }
    void sev() { emit(0xBF40); }
    void wfe() { emit(0xBF20); }
    void literal(uint32_t v) {  // (aligned to a word; the caller has padded)
        emit(static_cast<uint16_t>(v));
        emit(static_cast<uint16_t>(v >> 16));
    }
    // wait until FIFO_ST.VLD (r1 and r2 are scratch, r0 is the SIO base)
    void wait_valid() {
        const int top = here();
        ldr(1, 0, FIFO_ST);
        movs(2, 1);
        tst(1, 2);
        beq(top);
    }
};

static void load(uint32_t address, const Asm& a) {
    for (size_t i = 0; i < a.h.size(); ++i) std::memcpy(sram + (address - 0x20000000) + 2 * i, &a.h[i], 2);
}

// Core 1's program: r0 = SIO; forever { wait for a word; send it back plus one }.
static Asm core1_echo_plus_one() {
    Asm a;
    a.ldr_pc(0, 4);  // (the literal is at halfword 10; pc = base + 4, so 16 bytes on = 4 words)
    const int loop = a.here();
    a.wait_valid();
    a.ldr(3, 0, FIFO_RD);
    a.adds(3, 1);
    a.str(3, 0, FIFO_WR);
    a.b(loop);
    a.nop();
    a.literal(kSio);
    return a;
}

// Core 0's program: launch core 1 with the handshake of datasheet 2.8.2, send 5, wait for the answer (6) into r7, then spin.
static Asm core0_launch_and_ping() {
    Asm a;
    a.ldr_pc(0, 0);   // r0 = SIO     (literal 0 at halfword 20 .. fixed up below)
    a.ldr_pc(4, 0);   // r4 = vector table
    a.ldr_pc(5, 0);   // r5 = stack
    a.ldr_pc(6, 0);   // r6 = entry
    auto send_reg = [&](int reg) {
        a.str(reg, 0, FIFO_WR);
        a.wait_valid();
        a.ldr(1, 0, FIFO_RD);  // (the echo)
    };
    a.movs(3, 0);
    send_reg(3);
    send_reg(3);
    a.movs(3, 1);
    send_reg(3);
    send_reg(4);
    send_reg(5);
    send_reg(6);
    a.movs(3, 5);
    a.str(3, 0, FIFO_WR);
    a.wait_valid();
    a.ldr(7, 0, FIFO_RD);
    const int spin = a.here();
    a.b(spin);
    if (a.here() & 1) a.nop();
    const int pool = a.here();
    a.literal(kSio);
    a.literal(kCode0);
    a.literal(kStack1);
    a.literal(kCode1 | 1);
    // the four ldr-from-pool (r0, r4, r5, r6 = the literals in order): each a word offset from its own aligned pc
    const int regs[4] = {0, 4, 5, 6};
    for (int i = 0; i < 4; ++i) {
        const int pc = (2 * i + 4) & ~3;
        const int literal = 2 * (pool + 2 * i);
        a.h[i] = static_cast<uint16_t>(0x4800 | regs[i] << 8 | (literal - pc) / 4);
    }
    return a;
}

struct Rig {
    Mcu m;
    Rig() {
        McuMemory memory;
        memory.rom = rom;
        memory.flash = flash;
        memory.flash_size = sizeof flash;
        memory.sram = sram;
        memory.dpram = dpram;
        m.set_memory(memory);
        McuHost host;
        host.failed = &g_host_failed;
        host.warn = [](void*, uint32_t, uint32_t, uint32_t, int64_t) {};
        m.build(host);
        m.set_core1_enabled(true);
        CHECK(m.reset(false, true));
    }
};

// Every access to the SIO, with the time the clock had when it happened and who made it: what "ordered by time" is checked on.
struct TraceEntry {
    int64_t ticks;
    uint8_t core;
    uint32_t offset;
    uint32_t value;
    bool write;
    bool operator==(const TraceEntry& o) const { return ticks == o.ticks && core == o.core && offset == o.offset && value == o.value && write == o.write; }
};
static std::vector<TraceEntry> g_trace;
static WindowHandler g_inner_sio;
static Mcu* g_traced = nullptr;
static uint32_t traced_read(void* ctx, uint32_t offset) {
    const uint32_t v = g_inner_sio.read32(ctx, offset);
    g_trace.push_back({g_traced->clock().ticks(), g_traced->bus.initiator, offset, v, false});
    return v;
}
static void traced_write(void* ctx, uint32_t offset, int64_t value, uint32_t atomic) {
    g_trace.push_back({g_traced->clock().ticks(), g_traced->bus.initiator, offset, static_cast<uint32_t>(value), true});
    g_inner_sio.write32(ctx, offset, value, atomic);
}

static void test_fifo_registers_and_the_launch_handshake() {
    Rig r;
    Mcu& m = r.m;
    CHECK(!m.core1_running() && m.bus.initiator == 0);
    // FIFO_ST of core 0: nothing to read, room to write.
    CHECK((m.bus.read32(kSio + FIFO_ST) & 0xF) == 0b0010);
    // The handshake: 0, 0, 1 and the three values are each echoed; the last one launches core 1.
    const uint32_t words[6] = {0, 0, 1, kCode0, kStack1, kCode1 | 1};
    for (int i = 0; i < 6; ++i) {
        m.bus.write32(kSio + FIFO_WR, words[i]);
        CHECK((m.bus.read32(kSio + FIFO_ST) & 1) == 1);  // VLD: the echo
        CHECK(m.bus.read32(kSio + FIFO_RD) == words[i]);
        CHECK(i < 5 ? !m.core1_running() : m.core1_running());
    }
    CHECK(m.cpu1.registers[15] == (kCode1 & ~1u) && m.cpu1.registers[13] == kStack1 && m.cpu1.vtor == kCode0);
    // A wrong word restarts the sequence.
    Rig r2;
    Mcu& n = r2.m;
    for (uint32_t w : {0u, 0u, 7u, 0u, 0u, 1u, kCode0, kStack1}) {
        n.bus.write32(kSio + FIFO_WR, w);
        (void)n.bus.read32(kSio + FIFO_RD);
    }
    CHECK(!n.core1_running());  // (7 restarted it, so the sequence is not complete yet)
    n.bus.write32(kSio + FIFO_WR, kCode1 | 1);
    CHECK(n.core1_running());
    // Core 1's side: its writes go to the 1 -> 0 FIFO, its reads come from the 0 -> 1 one. Reading an empty FIFO sets ROE, writing a full one WOF; writing FIFO_ST clears them.
    Mcu& k = r.m;
    k.bus.initiator = 1;
    CHECK((k.bus.read32(kSio + FIFO_ST) & 0b1111) == 0b0010);
    CHECK(k.bus.read32(kSio + FIFO_RD) == 0 && (k.bus.read32(kSio + FIFO_ST) & 0b1000) != 0);  // ROE
    k.bus.write32(kSio + FIFO_ST, 0);
    CHECK((k.bus.read32(kSio + FIFO_ST) & 0b1000) == 0);
    for (int i = 0; i < 8; ++i) k.bus.write32(kSio + FIFO_WR, 100 + i);
    CHECK((k.bus.read32(kSio + FIFO_ST) & 0b0110) == 0);  // full: not RDY, no WOF yet
    k.bus.write32(kSio + FIFO_WR, 999);
    CHECK((k.bus.read32(kSio + FIFO_ST) & 0b0100) != 0);  // WOF
    k.bus.initiator = 0;
    CHECK((k.bus.read32(kSio + FIFO_ST) & 1) == 1);  // core 0 sees VLD
    for (int i = 0; i < 8; ++i) CHECK(k.bus.read32(kSio + FIFO_RD) == static_cast<uint32_t>(100 + i));  // in order, and the 999 was not stored
    CHECK((k.bus.read32(kSio + FIFO_ST) & 1) == 0);
    // CPUID follows the initiator.
    CHECK(k.bus.read32(kSio) == 0);
    k.bus.initiator = 1;
    CHECK(k.bus.read32(kSio) == 1);
    k.bus.initiator = 0;
}

static void run_until(Mcu& m, uint32_t quantum, int max_batches, bool (*done)(Mcu&)) {
    McuLimits limits;
    limits.instruction_ceiling = 2000;
    limits.quantum_cycles = static_cast<int>(quantum);
    limits.yield_budget_seconds = 1e9;
    for (int i = 0; i < max_batches && !done(m); ++i) CHECK(m.run_at_sys_clock(limits) == 0);
}

static void test_two_cores_ping_pong(uint32_t quantum, std::vector<TraceEntry>* trace) {
    Rig r;
    Mcu& m = r.m;
    load(kCode0, core0_launch_and_ping());
    load(kCode1, core1_echo_plus_one());
    m.cpu.registers[15] = kCode0;
    m.cpu.registers[13] = 0x20004000;
    if (trace != nullptr) {
        g_trace.clear();
        g_traced = &m;
        g_inner_sio = m.sio().window_handler();
        WindowHandler wrapped = g_inner_sio;
        wrapped.read32 = traced_read;
        wrapped.write32 = traced_write;
        m.bus.set_sio(wrapped);
    }
    run_until(m, quantum, 200, [](Mcu& x) { return x.cpu.registers[7] == 6; });
    CHECK(m.core1_running());
    CHECK(m.cpu.registers[7] == 6);  // core 0 sent 5 and core 1 answered 6
    CHECK(m.cpu1.registers[3] == 6);
    if (trace != nullptr) *trace = g_trace;
}

static void test_the_f2_scheduler_orders_every_device_access_like_the_exact_one() {
    std::vector<TraceEntry> exact, quantum;
    test_two_cores_ping_pong(1, &exact);
    test_two_cores_ping_pong(16, &quantum);
    CHECK(!exact.empty());
    CHECK(exact.size() == quantum.size());
    bool same = exact.size() == quantum.size();
    for (size_t i = 0; same && i < exact.size(); ++i) same = exact[i] == quantum[i];
    CHECK(same);
    if (!same) {
        for (size_t i = 0; i < exact.size() && i < quantum.size(); ++i) {
            if (!(exact[i] == quantum[i])) {
                std::printf("first difference at %zu: exact (t=%lld c%u off=%x v=%x w=%d) quantum (t=%lld c%u off=%x v=%x w=%d)\n", i, (long long)exact[i].ticks, exact[i].core, exact[i].offset, exact[i].value,
                            exact[i].write, (long long)quantum[i].ticks, quantum[i].core, quantum[i].offset, quantum[i].value, quantum[i].write);
                break;
            }
        }
    }
}

static void test_sev_wakes_a_core_asleep_in_a_wfe() {
    Rig r;
    Mcu& m = r.m;
    // Core 1 is "running" at a WFE; core 0 executes SEV. (Without core 0's event the WFE would sleep for ever.)
    Asm a1;
    a1.wfe();
    a1.movs(1, 42);
    a1.b(a1.here());
    load(kCode1, a1);
    Asm a0;
    for (int i = 0; i < 20; ++i) a0.nop();  // (give core 1 time to reach its WFE first)
    a0.sev();
    a0.b(a0.here());
    load(kCode0, a0);
    m.cpu.registers[15] = kCode0;
    m.cpu.registers[13] = 0x20004000;
    CHECK(m.launch_core1(kCode0, kStack1, kCode1 | 1));
    McuLimits limits;
    limits.instruction_ceiling = 500;
    limits.yield_budget_seconds = 1e9;
    CHECK(m.run_at_sys_clock(limits) == 0);
    CHECK(m.cpu1.registers[1] == 42);
}

int main() {
    test_fifo_registers_and_the_launch_handshake();
    test_two_cores_ping_pong(16, nullptr);
    test_the_f2_scheduler_orders_every_device_access_like_the_exact_one();
    test_sev_wakes_a_core_asleep_in_a_wfe();
    if (failures == 0) std::printf("test_core1: ok\n");
    return failures == 0 ? 0 : 1;
}
