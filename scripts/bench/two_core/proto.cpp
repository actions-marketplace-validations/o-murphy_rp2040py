// Throwaway prototype for docs/records/0104 (P0.5) - kept so that the figures in that record can be reproduced (build and run: see gen.py): what does it cost to schedule two Cortex-M0+ cores on ONE thread, and how late does each scheme let the clock's alarms be for a core?
// Not product code. Two `Cpu` objects share the `Mcu`'s bus and clock; the loops below are the candidates; the workloads are hand-assembled Thumb programs (gen.py).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rp2040.hpp"

using namespace rp2040core;

namespace {
constexpr int64_t kCycle = 128;                       // ticks per system clock cycle (125 MHz)
constexpr uint32_t kCore1At = 0x10000, kSharedAt = 0x20000;
uint8_t g_rom[16 * 1024], g_sram[264 * 1024], g_dpram[4 * 1024];
std::vector<uint8_t> g_flash(16 * 1024 * 1024, 0xFF);
Mcu g_mcu;
Cpu g_cpu1;
int g_failed = 0;

struct Core {
    Cpu* cpu;
    int64_t local = 0;   // ticks
    uint64_t n = 0;      // instructions
};
Core g_core[2] = {{&g_mcu.cpu}, {&g_cpu1}};
bool g_two = true;
uint32_t g_locks = 0;  // the SIO spinlock state a run starts with
bool g_sync_clock = false;  // candidate F2: every device access also brings the clock to the accessing core's time
int64_t g_horizon = 0;
std::vector<uint32_t> g_trace;  // the TIMELR values core 0 read

// the accuracy probe: a periodic alarm that records how far ahead of the clock each core's own time already is when the alarm fires
Alarm g_alarm;
int64_t g_skew = 0;
uint64_t g_fires = 0;
bool on_alarm(void*) {
    const int64_t now = g_mcu.clock().ticks();
    for (int i = 0; i < (g_two ? 2 : 1); ++i) {
        if (g_core[i].cpu->waiting) continue;
        const int64_t d = g_core[i].local > now ? g_core[i].local - now : now - g_core[i].local;
        if (d > g_skew) g_skew = d;
    }
    ++g_fires;
    g_mcu.clock().schedule_ticks(&g_alarm, 1000 * kCycle);
    return true;
}

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> v;
    if (std::FILE* f = std::fopen(path, "rb")) {
        uint8_t buf[256];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "cannot read %s\n", path);
        std::exit(2);
    }
    return v;
}

void setup_chip() {
    McuMemory m;
    m.rom = g_rom;
    m.flash = g_flash.data();
    m.sram = g_sram;
    m.dpram = g_dpram;
    g_mcu.set_memory(m);
    McuHost h;
    h.failed = &g_failed;
    h.warn = [](void*, uint32_t, uint32_t, uint32_t, int64_t) {};
    g_mcu.build(h);
    g_mcu.set_crystal_hz(12e6);
    g_mcu.reset(false, true);
    g_mcu.leave_reset();
    CpuHost ch;
    ch.failed = &g_failed;
    g_cpu1.init(&g_mcu.bus, ch, kMaxHardwareIrq);
}

void load(const char* p0, const char* p1) {  // p1 may be null: a single-core run
    std::memset(g_sram, 0, sizeof g_sram);
    auto a = read_file(p0);
    std::memcpy(g_sram, a.data(), a.size());
    g_two = p1 != nullptr;
    g_mcu.cpu.reset();
    g_mcu.cpu.registers[15] = 0x20000000;
    g_mcu.cpu.registers[13] = 0x20040000;
    if (g_two) {
        auto b = read_file(p1);
        std::memcpy(g_sram + kCore1At, b.data(), b.size());
        g_cpu1.reset();
        g_cpu1.registers[15] = 0x20000000 + kCore1At;
        g_cpu1.registers[13] = 0x2003F000;
    }
    Clock& clk = g_mcu.clock();
    g_mcu.sio().spin_lock = g_locks;
    clk.cancel(&g_alarm);
    g_alarm.fire = on_alarm;
    g_skew = 0;
    g_fires = 0;
    for (auto& c : g_core) { c.local = clk.ticks(); c.n = 0; }
    { const int64_t dt = (16000 - clk.ticks() % 16000) % 16000; if (dt > 0) clk.tick_ticks(dt); }  // every run starts on a microsecond boundary
    for (auto& c : g_core) { c.local = clk.ticks(); }
    g_trace.clear();
    clk.schedule_ticks(&g_alarm, 1000 * kCycle);
}

inline void exec1(Core& c) {
    const int d = c.cpu->execute();
    if (d < 0) { std::fprintf(stderr, "fault\n"); std::exit(1); }
    c.local += static_cast<int64_t>(d) * kCycle;
    ++c.n;
}
inline bool w(int i) { return !g_two ? (i == 1) : g_core[i].cpu->waiting; }  // a core that does not exist waits for ever

// ---- the SIO flag: a wrapper over the SIO's window handler that notes every access (candidate E ends a quantum at one) ----
bool g_hit = false;
bool g_exact = false;  // candidate F: an SIO access first lets the other core catch up to this core's time
int g_cur = 0;         // the core whose instruction is executing
struct SioWrap { WindowHandler inner; };
SioWrap g_wrap;
void catch_up();
void sync_device();
uint32_t sio_read(void* ctx, uint32_t off) { g_hit = true; if (g_exact) sync_device(); auto* w = static_cast<SioWrap*>(ctx); return w->inner.read32(w->inner.ctx, off); }
void sio_write(void* ctx, uint32_t off, int64_t v, uint32_t at) { g_hit = true; if (g_exact) sync_device(); auto* w = static_cast<SioWrap*>(ctx); w->inner.write32(w->inner.ctx, off, v, at); }

// ---- the candidates ----
void run_S(int64_t target) {  // one core, one instruction, one clock tick: the shape of today's loop
    Clock& clk = g_mcu.clock();
    Core& c = g_core[0];
    while (clk.ticks() < target) {
        const int d = c.cpu->execute();
        if (d < 0) std::exit(1);
        c.local += static_cast<int64_t>(d) * kCycle;
        ++c.n;
        clk.tick_ticks(static_cast<int64_t>(d) * kCycle);
    }
}
int pick(void) {
    if (w(0) && w(1)) return -1;
    if (w(0)) return 1;
    if (w(1)) return 0;
    return g_core[1].local < g_core[0].local ? 1 : 0;
}
void run_A(int64_t target) {  // the core with the smaller local time runs one instruction; the clock is ticked to that time first
    Clock& clk = g_mcu.clock();
    while (clk.ticks() < target) {
        const int p = pick();
        if (p < 0) break;
        g_cur = p;
        const int64_t dt = g_core[p].local - clk.ticks();
        if (dt > 0) clk.tick_ticks(dt);
        exec1(g_core[p]);
    }
}
void run_Ab(int64_t target) {  // the same, but the chosen core keeps running while its time does not pass the other's
    Clock& clk = g_mcu.clock();
    while (clk.ticks() < target) {
        const int p = pick();
        if (p < 0) break;
        const int o = 1 - p;
        const int64_t limit = w(o) ? INT64_MAX : g_core[o].local;
        do {
            const int64_t dt = g_core[p].local - clk.ticks();
            if (dt > 0) clk.tick_ticks(dt);
            exec1(g_core[p]);
        } while ((p == 0 ? g_core[p].local <= limit : g_core[p].local < limit) && clk.ticks() < target);
    }
}
void run_B(int64_t target) {  // strict alternation, one instruction each; a round is as long as the longer instruction
    Clock& clk = g_mcu.clock();
    while (clk.ticks() < target) {
        int d0 = 0, d1 = 0;
        if (w(0) && w(1)) break;
        if (!w(0)) { d0 = g_core[0].cpu->execute(); g_core[0].local += d0 * kCycle; ++g_core[0].n; }
        if (!w(1)) { d1 = g_core[1].cpu->execute(); g_core[1].local += d1 * kCycle; ++g_core[1].n; }
        clk.tick_ticks(static_cast<int64_t>(std::max(d0, d1)) * kCycle);
    }
}
void run_C(int64_t target, int quantum) {  // each core runs to the end of a quantum (cut at the next alarm); the clock is ticked once per quantum
    Clock& clk = g_mcu.clock();
    while (clk.ticks() < target) {
        const int64_t now = clk.ticks();
        int64_t horizon = now + static_cast<int64_t>(quantum) * kCycle;
        if (clk.has_alarm()) horizon = std::min(horizon, now + clk.ticks_to_next_alarm());
        horizon = std::max(horizon, now + kCycle);
        if (w(0) && w(1)) break;
        int64_t t = INT64_MAX;
        for (int i = 0; i < 2; ++i) {
            if (w(i)) continue;
            g_cur = i;
            while (g_core[i].local < horizon) exec1(g_core[i]);
            t = std::min(t, g_core[i].local);
        }
        if (t > now) clk.tick_ticks(t - now);
    }
}

SioWrap g_twrap;
uint32_t timer_read(void* ctx, uint32_t off) {
    if (g_exact && g_sync_clock) sync_device();
    auto* w = static_cast<SioWrap*>(ctx);
    const uint32_t v = w->inner.read32(w->inner.ctx, off);
    if (g_cur == 0 && off == 0x0C && g_trace.size() < 200000) g_trace.push_back(v);
    return v;
}
void timer_write(void* ctx, uint32_t off, int64_t v, uint32_t at) { auto* w = static_cast<SioWrap*>(ctx); w->inner.write32(w->inner.ctx, off, v, at); }
void catch_up() {  // runs inside the SIO access of core g_cur: the other core, if it is behind, executes up to this core's time first, so the access is ordered by time
    const int p = g_cur, q = 1 - p;
    if (w(q) || g_core[q].local >= g_core[p].local) return;
    g_cur = q;
    while (g_core[q].local < g_core[p].local) exec1(g_core[q]);
    g_cur = p;
}
void sync_device() {  // F / F2: order a device access by time (the other core catches up first) and, for F2, bring the clock to this core's time
    catch_up();
    if (!g_sync_clock) return;
    Clock& clk = g_mcu.clock();
    const int64_t dt = g_core[g_cur].local - clk.ticks();
    if (dt > 0) clk.tick_ticks(dt);
    if (clk.has_alarm()) g_horizon = std::min(g_horizon, clk.ticks() + clk.ticks_to_next_alarm());
}
void run_F(int64_t target, int quantum) {  // a quantum (cut at the next alarm) with the exact ordering of every SIO access; with g_sync_clock also of the TIMER's (F2)
    Clock& clk = g_mcu.clock();
    g_exact = true;
    while (clk.ticks() < target) {
        const int64_t now = clk.ticks();
        g_horizon = now + static_cast<int64_t>(quantum) * kCycle;
        if (clk.has_alarm()) g_horizon = std::min(g_horizon, now + clk.ticks_to_next_alarm());
        g_horizon = std::max(g_horizon, now + kCycle);
        if (w(0) && w(1)) break;
        int64_t t = INT64_MAX;
        for (int i = 0; i < 2; ++i) {
            if (w(i)) continue;
            g_cur = i;
            while (g_core[i].local < g_horizon) exec1(g_core[i]);
            t = std::min(t, g_core[i].local);
        }
        const int64_t dt = t - clk.ticks();
        if (dt > 0) clk.tick_ticks(dt);
    }
    g_exact = false;
}
void run_E(int64_t target, int quantum) {  // a quantum cut at the next alarm and ended early right after a core touches the SIO; the other core then catches up to that time
    Clock& clk = g_mcu.clock();
    while (clk.ticks() < target) {
        if (w(0) && w(1)) break;
        int64_t t = INT64_MAX;
        for (int i = 0; i < 2; ++i) if (!w(i)) t = std::min(t, g_core[i].local);
        const int64_t now = clk.ticks();
        if (t > now) clk.tick_ticks(t - now);
        int64_t horizon = t + static_cast<int64_t>(quantum) * kCycle;
        if (clk.has_alarm()) horizon = std::min(horizon, clk.ticks() + clk.ticks_to_next_alarm());
        horizon = std::max(horizon, t + kCycle);
        const int p = pick();
        g_hit = false;
        while (g_core[p].local < horizon && !g_hit) exec1(g_core[p]);
        if (g_hit) {
            const int q = 1 - p;
            if (!w(q)) {
                const int64_t at = g_core[p].local;
                g_hit = false;
                while (g_core[q].local < at && !g_hit) exec1(g_core[q]);
            }
        }
    }
}

struct Result { double wall; uint64_t n0, n1; int64_t sim_ticks; int64_t skew; uint64_t fires; uint32_t shared; };
template <class F>
Result measure(const char* p0, const char* p1, int64_t sim_cycles, F&& loop) {
    Result best{1e9, 0, 0, 0, 0, 0, 0};
    for (int rep = 0; rep < 3; ++rep) {
        load(p0, p1);
        Clock& clk = g_mcu.clock();
        const int64_t start = clk.ticks();
        const int64_t target = start + sim_cycles * kCycle;
        const auto t0 = std::chrono::steady_clock::now();
        loop(target);
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (wall < best.wall) {
            uint32_t shared;
            std::memcpy(&shared, g_sram + kSharedAt, 4);
            best = Result{wall, g_core[0].n, g_core[1].n, clk.ticks() - start, g_skew, g_fires, shared};
        }
    }
    return best;
}
void report(const char* wl, const char* name, const Result& r, bool pingpong) {
    const double mips = static_cast<double>(r.n0 + r.n1) / r.wall / 1e6;
    const double sim_ms = static_cast<double>(r.sim_ticks) / kCycle / 125e3;
    std::printf("%-12s %-22s %7.1f Minstr/s  (core0 %9llu, core1 %9llu)  skew<=%5.1f cyc", wl, name, mips, (unsigned long long)r.n0, (unsigned long long)r.n1, static_cast<double>(r.skew) / kCycle);
    if (pingpong) std::printf("  %7.2f round trips per simulated ms", r.shared / sim_ms);
    std::printf("\n");
}
}  // namespace

int main() {
    setup_chip();
    g_wrap.inner = g_mcu.sio().window_handler();
    g_mcu.bus.set_sio(WindowHandler{sio_read, sio_write, &g_wrap});
    g_twrap.inner = g_mcu.timer().window_handler();
    g_mcu.bus.windows.attach(0x40054000, WindowHandler{timer_read, timer_write, &g_twrap});
    g_mcu.bus.write32(0x4000C000, 0);                 // RESETS: no block held in reset
    g_mcu.bus.write32(0x40024000, 0xFABAA0);          // XOSC CTRL: enabled
    g_mcu.bus.write32(0x40008030, 2);                 // CLK_REF_CTRL: the crystal
    g_mcu.bus.write32(0x4005802C, 12 | (1 << 9));    // WATCHDOG TICK: 12 cycles per microsecond, enabled
    const int64_t T = 8 * 1000 * 1000;  // simulated cycles per run (64 ms at 125 MHz)
    struct Cand { const char* name; void (*fn)(int64_t); int q; };
    auto go = [&](const char* wl, const char* p0, const char* p1, bool pp) {
        if (p1 == nullptr) {
            report(wl, "S  one core, today's shape", measure(p0, p1, T, [](int64_t t) { run_S(t); }), pp);
            return;
        }
        report(wl, "A  local time/instr", measure(p0, p1, T, [](int64_t t) { run_A(t); }), pp);
        report(wl, "A' local time/burst", measure(p0, p1, T, [](int64_t t) { run_Ab(t); }), pp);
        report(wl, "B  alternation", measure(p0, p1, T, [](int64_t t) { run_B(t); }), pp);
        for (int q : {16, 64, 256, 1024}) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "C  quantum %d cyc", q);
            report(wl, nm, measure(p0, p1, T, [q](int64_t t) { run_C(t, q); }), pp);
        }
    };
    std::printf("-- W0: one core running the ALU loop (the baseline)\n");
    go("W0 1 core", "alu0.bin", nullptr, false);
    std::printf("-- W1: two cores, both busy (two ALU loops)\n");
    go("W1 2 busy", "alu0.bin", "alu1.bin", false);
    std::printf("-- W3: core 0 busy, core 1 asleep in WFI\n");
    go("W3 1+idle", "alu0.bin", "wfi.bin", false);
    std::printf("-- W2: ping-pong through shared RAM (each core polls for the other)\n");
    go("W2 pingpong", "pp0.bin", "pp1.bin", true);
    std::printf("-- W4: ping-pong through the SIO spinlocks (the way SDK firmware synchronises); both locks held at the start\n");
    g_locks = 3;
    report("W4 spinlock", "A  local time/instr", measure("sl0.bin", "sl1.bin", T, [](int64_t t) { run_A(t); }), true);
    for (int q : {16, 64, 256, 1024}) {
        char nm[32];
        std::snprintf(nm, sizeof nm, "C  quantum %d cyc", q);
        report("W4 spinlock", nm, measure("sl0.bin", "sl1.bin", T, [q](int64_t t) { run_C(t, q); }), true);
    }
    for (int q : {16, 64, 256, 1024}) {
        char nm[32];
        std::snprintf(nm, sizeof nm, "E  quantum %d + SIO cut", q);
        report("W4 spinlock", nm, measure("sl0.bin", "sl1.bin", T, [q](int64_t t) { run_E(t, q); }), true);
    }
    for (int q : {16, 64, 256, 1024}) {
        char nm[32];
        std::snprintf(nm, sizeof nm, "F  quantum %d + exact SIO", q);
        report("W4 spinlock", nm, measure("sl0.bin", "sl1.bin", T, [q](int64_t t) { run_F(t, q); }), true);
    }
    std::printf("-- W5: accuracy of the clock inside a quantum: core 0 reads the TIMER's microsecond counter in a loop, core 1 runs the ALU loop; the sequence of readings must equal A's\n");
    g_locks = 0;
    std::vector<uint32_t> ref;
    auto cmp = [&](const char* nm, void (*fn)(int64_t, int), int q, bool sync) {
        g_sync_clock = sync;
        const Result r = measure("tm0.bin", "alu1.bin", 2 * 1000 * 1000, [&](int64_t t) { fn(t, q); });
        const size_t n = std::min(ref.size(), g_trace.size());
        size_t bad = 0, first = (size_t)-1;
        for (size_t i = 0; i < n; ++i) if (g_trace[i] - g_trace[0] != ref[i] - ref[0]) { ++bad; if (first == (size_t)-1) first = i; }
        std::printf("W5 timer     %-26s %6zu readings compared, %6zu differ%s  (%.1f Minstr/s)\n", nm, n, bad, bad ? "" : "  - identical to A", static_cast<double>(r.n0 + r.n1) / r.wall / 1e6);
        (void)first;
    };
    g_sync_clock = false;
    measure("tm0.bin", "alu1.bin", 2 * 1000 * 1000, [](int64_t t) { run_A(t); });
    ref = g_trace;
    std::printf("   (A: readings 0..4 = %u %u %u %u %u, reading 199999 = %u: the counter %s)\n", ref[0], ref[1], ref[2], ref[3], ref[4], ref.back(), ref.back() != ref[0] ? "counts" : "DOES NOT COUNT");
    std::printf("W5 timer     A  (the reference)         %6zu readings\n", ref.size());
    for (int q : {16, 256}) {
        char nm[40];
        std::snprintf(nm, sizeof nm, "C  quantum %d", q);
        cmp(nm, [](int64_t t, int qq) { run_C(t, qq); }, q, false);
    }
    for (int q : {16, 256, 1024}) {
        char nm[40];
        std::snprintf(nm, sizeof nm, "F  quantum %d", q);
        cmp(nm, run_F, q, false);
    }
    for (int q : {16, 256, 1024}) {
        char nm[40];
        std::snprintf(nm, sizeof nm, "F2 quantum %d (+clock sync)", q);
        cmp(nm, run_F, q, true);
    }
    return 0;
}
