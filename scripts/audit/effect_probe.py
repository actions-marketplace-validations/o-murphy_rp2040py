"""Does writing a register change anything observable? A static scan behind scripts/audit/register_status.py: for every register of a block it snapshots the chip, writes the register on a second chip and compares
(the registers and the pins). A register whose write changes nothing is a candidate for the Stub list. Usage: python scripts/audit/effect_probe.py rp2040.txt [BLOCK ...]  (rp2040.txt is the datasheet as text, see
docs/reference/core-block-recipe.md, section A)."""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import datasheet_conformance as D
import register_status as ST
from rp2040py.rp2040 import RP2040

DS = sys.argv[1]
only = set(sys.argv[2:])
regs = D.parse(DS)
by = {}
for r in regs:
    by.setdefault(r["block"], []).append(r)
SKIP = re.compile(r"(CLR|ICR|FIFO|RXF|TXF|DR$|^DR|DATA_CMD|RESULT|TIMEHW|TIMELW|TIMERAWH|TIMERAWL|TIMEH$|TIMEL$|DR\d|ECR)", re.I)

def snapshot(chip, block, skip_addr):
    out = {}
    for r in by[block]:
        a = D.BASES[block] + int(r["offset"], 16)
        if a == skip_addr or SKIP.search(r["name"]):
            continue
        try:
            out[a] = chip.read_uint32(a)
        except Exception as e:
            out[a] = repr(e)
    out["nvic"] = chip.read_uint32(0xE000E200)
    out["clk"] = (chip.clk_sys, chip.clk_peri)
    try:
        out["pins"] = tuple(int(p.value) if hasattr(p, "value") else 0 for p in chip.pins)
    except Exception:
        pass
    return out

def run(block, r, pattern):
    a = D.BASES[block] + int(r["offset"], 16)
    res = []
    for test in (False, True):
        chip = RP2040()
        chip.logger.warning = lambda *_: None
        if test:
            try:
                chip.write_uint32(a, pattern)
            except Exception as e:
                return f"raises {type(e).__name__}"
        chip.clock.tick(200_000)
        res.append(snapshot(chip, block, a))
    ctl, tst = res
    diff = [k for k in ctl if ctl[k] != tst[k]]
    return diff

for block in D.BASES:
    if only and block not in only: continue
    if block not in by: continue
    for r in by[block]:
        off = int(r["offset"], 16)
        st, _ = ST.state(block, off)
        if st != "Acts": continue
        writable = any(f[2] in D.WRITABLE for f in r["fields"])
        if not writable: continue
        effects = []
        for pat in (0xFFFFFFFF, 0x1, 0x55555555):
            d = run(block, r, pat)
            if isinstance(d, str): effects.append(d); break
            if d: effects.append(len(d)); break
        if not effects:
            print(f"NOEFFECT {block}.{r['name']} {r['offset']}")
