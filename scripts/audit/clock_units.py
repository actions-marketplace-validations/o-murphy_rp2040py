"""Rounding of the periods the blocks schedule, for candidate integer time units (docs/records/0100-integer-clock.md, "Measurements").

Run with `python scripts/audit/clock_units.py`. Pure arithmetic on exact fractions: no chip is built. The cases are the periods the blocks of the model schedule (a CPU cycle at the usual
system clocks, a UART frame, an SPI byte, a DMA word, the RTC second, the TIMER tick, the XIP stream word, the ROSC cycle, a WS2812 bit).
"""

from fractions import Fraction

UNITS = {"1 ns": 1, "1/16 ns": 16, "1/32 ns": 32, "1/64 ns": 64, "1 ps (1000/ns)": 1000}
I64 = 2**63

cases = []  # (label, period in ns as a Fraction, how often it recurs per simulated second)
for mhz in (125, 133, 200, 250, 48, 12, 6.5, 266):
    hz = Fraction(mhz).limit_denominator(1000) * 10**6
    cases.append((f"CPU cycle @ {mhz} MHz", 10**9 / hz, hz))
for baud in (9600, 115200, 230400, 460800, 921600, 1_000_000, 3_000_000):
    bits = Fraction(10)  # a 8N1 frame
    cases.append((f"UART frame @ {baud} baud", bits * 10**9 / baud, Fraction(baud) / bits))
for name, div in (
    ("SPI 125M/(2*1)", 2),
    ("SPI 125M/(4*3)", 12),
    ("SPI 125M/(254*256)", 254 * 256),
    ("SPI 48M/(2*5)", None),
):
    clk = Fraction(48_000_000) if div is None else Fraction(125_000_000)
    d = 10 if div is None else div
    frame = 8  # a byte
    cases.append((name + " byte", frame * d * 10**9 / clk, clk / d / frame))
cases.append(("DMA word @ 125 MHz", Fraction(8), Fraction(125_000_000)))
cases.append(("DMA word @ 133 MHz", 10**9 / Fraction(133_000_000), Fraction(133_000_000)))
cases.append(("RTC 1 s @ 46875 Hz (div 46875)", Fraction(10**9), Fraction(1)))
cases.append(("RTC 1 s @ 32768 Hz", Fraction(10**9), Fraction(1)))
cases.append(("RTC tick @ 46875 Hz /(div 3)", Fraction(3) * 10**9 / 46875, Fraction(46875, 3)))
cases.append(("TIMER 1 us", Fraction(1000), Fraction(10**6)))
cases.append(("XIP stream word SCKDV 2 @125M", Fraction(2 * 32) * 10**9 / 125_000_000, Fraction(125_000_000, 64)))
cases.append(("XIP stream word SCKDV 6 @133M", Fraction(6 * 32) * 10**9 / 133_000_000, Fraction(133_000_000, 192)))
cases.append(("ROSC cycle (6.5 MHz nominal)", 10**9 / Fraction(6_500_000), Fraction(6_500_000)))
cases.append(("ws2812 bit 1.25 us", Fraction(1250), Fraction(800_000)))

print("range of an int64 in each unit:")
for u, k in UNITS.items():
    secs = Fraction(I64, k) / 10**9
    print(f"  {u:16s} {float(secs) / 86400 / 365.25:10.1f} years")
print()
hdr = f"{'case':36s}" + "".join(f"{u:>24s}" for u in UNITS)
print("per-event rounding error [ps]  /  drift when every delta is rounded independently [ppm]")
print(hdr)
worst: dict[str, tuple[float, float, str]] = {u: (0.0, 0.0, "") for u in UNITS}
for label, period, per_s in cases:
    row = f"{label:36s}"
    for u, k in UNITS.items():
        ticks = period * k
        r = round(ticks)
        err_ns = (Fraction(r) - ticks) / k
        err_ps = float(err_ns) * 1000
        ppm = float(err_ns / period) * 1e6
        row += f"{err_ps:>12.1f} {ppm:>9.0f}ppm "[:24].rjust(24)
        if abs(ppm) > abs(worst[u][1]):
            worst[u] = (err_ps, ppm, label)
    print(row)
print()
print("worst drift per unit (rounding each delta on its own):")
for u, (e, p, l) in worst.items():
    print(f"  {u:16s} {p:9.0f} ppm  ({l}, {e:.1f} ps/event)")
