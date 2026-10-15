#!/usr/bin/env python3
"""Mutation testing of a C++ core block: does ``tests/cpp/test_X.cpp`` notice every change of ``core/X.hpp``?

    python scripts/audit/cpp_mutants.py usb            # core/usb.hpp against tests/cpp/test_usb.cpp
    python scripts/audit/cpp_mutants.py usb --list     # print the mutants without running them

Every line of logic in the header (from its first ``class`` on) is changed by one textual rule at a time - an operator swapped (``==``/``!=``, ``&&``/``||``, ``<``/``<=``, ``+``/``-``, ``|``/``&``,
``true``/``false``, ``|=``/``&=~``, ``++``/``--``) or a statement deleted - and the block's own C++ checks are built (``-fsanitize=address,undefined -fno-sanitize-recover=all``, so an out-of-bounds
index or an overflow the change introduces is a failure too) and run. A mutant that does not compile is dropped; one the checks do not fail on SURVIVED. A survivor is either a gap in the checks (add
a test) or an *equivalent* change (``<`` against ``<=`` when both sides are equal, an initial value the block overwrites before it is read). The equivalents are listed with their reason in
``scripts/audit/cpp_mutants_equivalents.json`` under the block's name, keyed by ``"<line text> | <rule>"`` (not by line number, which moves); the script exits 1 if anything survives that is not listed
there, and says which listed entries no longer match anything (stale, so the file cannot rot into an excuse).

Not run by CI per push (hundreds of compiles); run it when a block's header or its checks change, and say so in the commit. ``--jobs`` sets the parallelism (default: the CPU count).
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "src" / "rp2040py" / "native" / "core"
TESTS = ROOT / "tests" / "cpp"
EQUIVALENTS = Path(__file__).with_name("cpp_mutants_equivalents.json")
FLAGS = [
    "-std=c++17",
    "-fno-exceptions",
    "-fno-rtti",
    "-ffp-contract=off",
    "-w",
    "-fsanitize=address,undefined",
    "-fno-sanitize-recover=all",
]

# (text, replacement); applied at every occurrence in a line of logic, one occurrence per mutant
RULES = [
    ("==", "!="), ("!=", "=="), ("&&", "||"), ("||", "&&"), ("<=", "<"), (">=", ">"), (" < ", " <= "), (" > ", " >= "),
    (" + ", " - "), ("|=", "&=~"), ("&= ~", "|= "), (" | ", " & "), ("true", "false"), ("false", "true"), ("^= 1u", "= 1u"), ("++", "--"),
]  # fmt: skip
# a statement that can be deleted: an assignment, a (void) call, or a call/return of a function
STATEMENT = re.compile(
    r"\s*(\w[\w\[\]\.>\-_ ]*(=|\|=|&=|\+\+|--|\^=)[^;]*|\(void\)[^;]*|return [^;]*|\w+\([^;]*\));\s*$"
)


def mutants(source: list[str]) -> list[tuple[int, str, str]]:
    """(line index, rule description, the mutated line) for every mutant of the lines from the first class on."""
    start = next((i for i, line in enumerate(source) if re.match(r"\s*(class|struct)\s+\w+.*\{", line)), 0)
    found = []
    for index in range(start, len(source)):
        line = source[index]
        stripped = line.strip()
        if not stripped or stripped.startswith(("//", "#")):
            continue
        code = line.split("//")[0]
        tail = line[len(code) :]
        for old, new in RULES:
            at = code.find(old)
            while at >= 0:
                found.append((index, f"{old} -> {new}", code[:at] + new + code[at + len(old) :] + tail))
                at = code.find(old, at + 1)
        if STATEMENT.match(code) and not code.strip().startswith(("return true", "return false")):
            found.append((index, "delete", "/*deleted*/;"))
    return found


def key(source: list[str], index: int, rule: str) -> str:
    return f"{source[index].split('//')[0].strip()} | {rule}"


def run_one(header: str, test: Path, source: list[str], mutant: tuple[int, str, str], cxx: str) -> str:
    index, _rule, text = mutant
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / header).write_text("\n".join(source[:index] + [text] + source[index + 1 :]))
        exe = Path(tmp) / "t"
        build = subprocess.run(
            [cxx, *FLAGS, f"-I{tmp}", f"-I{CORE}", str(test), "-o", str(exe)], capture_output=True, check=False
        )
        if build.returncode != 0:
            return "compile"
        try:
            run = subprocess.run([str(exe)], capture_output=True, check=False, timeout=30)
        except subprocess.TimeoutExpired:
            return "killed"
        return "survived" if run.returncode == 0 else "killed"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("block", help="the block's name: core/<block>.hpp against tests/cpp/test_<block>.cpp")
    parser.add_argument("--list", action="store_true", help="print the mutants and stop")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()
    header = f"{args.block}.hpp"
    test = TESTS / f"test_{args.block}.cpp"
    source = (CORE / header).read_text().split("\n")
    found = mutants(source)
    if args.list:
        for index, rule, text in found:
            print(f"{index + 1}: {rule}: {text.strip()}")
        print(len(found), "mutants")
        return 0
    cxx = next((name for name in ("c++", "g++", "clang++") if shutil.which(name)), None)
    if cxx is None:
        print("no C++ compiler on PATH", file=sys.stderr)
        return 2
    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda m: run_one(header, test, source, m, cxx), found))
    counts = {name: results.count(name) for name in ("killed", "survived", "compile")}
    equivalents = json.loads(EQUIVALENTS.read_text()).get(args.block, {}) if EQUIVALENTS.exists() else {}
    survivors = [(m, key(source, m[0], m[1])) for m, r in zip(found, results, strict=True) if r == "survived"]
    seen = {k for _, k in survivors}
    unexplained = [(m, k) for m, k in survivors if k not in equivalents]
    stale = sorted(set(equivalents) - seen)
    print(
        f"{args.block}: {len(found)} mutants - {counts['killed']} killed, {counts['compile']} do not compile, {counts['survived']} survived ({len(survivors) - len(unexplained)} listed as equivalent)"
    )
    for (index, rule, text), _k in unexplained:
        print(f"  SURVIVED {header}:{index + 1}: {rule}: {text.strip()}")
    for entry in stale:
        print(f"  STALE equivalent (matches nothing now): {entry}")
    return 1 if unexplained or stale else 0


if __name__ == "__main__":
    raise SystemExit(main())
