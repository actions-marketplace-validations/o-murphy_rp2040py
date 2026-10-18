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
    clk.cancel(&g_alarm);
    g_alarm.fire = on_alarm;
    g_skew = 0;
    g_fires = 0;
    for (auto& c : g_core) { c.local = clk.ticks(); c.n = 0; }
    clk.schedule_ticks(&g_alarm, 1000 * kCycle);
}

inline void exec1(Core& c) {
    const int d = c.cpu->execute();
    if (d < 0) { std::fprintf(stderr, "fault\n"); std::exit(1); }
    c.local += static_cast<int64_t>(d) * kCycle;
    ++c.n;
}
inline bool w(int i) { return !g_two ? (i == 1) : g_core[i].cpu->waiting; }  // a core that does not exist waits for ever

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
            while (g_core[i].local < horizon) exec1(g_core[i]);
            t = std::min(t, g_core[i].local);
        }
        if (t > now) clk.tick_ticks(t - now);
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
    return 0;
}
