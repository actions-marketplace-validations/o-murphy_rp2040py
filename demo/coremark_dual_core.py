#!/usr/bin/env python3
"""CoreMark on both cores of the emulated RP2040: builds the dual-core UF2 once, runs it through the
USB CDC console and exits on the result line.

Usage: python demo/coremark_dual_core.py [--image dual-core.uf2] [extra `rp2040py micropython` flags]

Why it builds the firmware instead of downloading it: the only upstream release with a second core,
protik09/CoreMark-RP2040 v1.0.7, does not benchmark anything. Its `core_start_parallel(0, ...)` runs
core 0's whole iteration *before* core 1 is launched, hands core 1 `&results` (the address of a local
pointer, not of the structure), and `core_end_parallel` has the wait for core 1 commented out - it
prints `Total ticks : 265`, `crcfinal 0x0000` and "ERROR! Must execute for at least 10 secs". The
emulator runs it faithfully (docs/records/0104-second-core-seams-and-architecture.md). `coremark_dual_core/dual-core.patch`
is the fix against that tag: core 1 is launched first on `results[1]`, core 0 runs `results[0]`
meanwhile, the end waits for core 1, and the two 2 KB data blocks live in .bss (4 KB on core 0's
stack overflowed it).

The build needs `git`, `cmake` and an `arm-none-eabi-gcc`; pico-sdk 2.1.1 and CoreMark v1.0.7 are
cloned into a temporary directory. The UF2 is cached as ~/.cache/rp2040py/coremark-dual-core.uf2;
`--image` runs any other build (or delete the cached file to rebuild).

Same two things as coremark_single_thread.py: an Enter is fed to the console after the device has
enumerated, and it stops on the `Iterations/Sec : <n>` line.

Expected: a valid result - about 12 s of simulated time, `Parallel Multicore : 2`, equal CRCs on both
cores (`[0]crcfinal` = `[1]crcfinal` = 0xcc42, the single-core build's), and roughly twice the
single-core score. The run takes several minutes of wall time: two busy cores, ~12 s of chip time.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from coremark_single_thread import RESULT_REGEX, _feed_enter_to_stdin

from rp2040py.cli import main

COREMARK_URL = "https://github.com/protik09/CoreMark-RP2040.git"
COREMARK_TAG = "v1.0.7"
PICO_SDK_URL = "https://github.com/raspberrypi/pico-sdk.git"
PICO_SDK_TAG = "2.1.1"
PATCH = Path(__file__).with_name("coremark_dual_core") / "dual-core.patch"
CACHED = Path.home() / ".cache" / "rp2040py" / "coremark-dual-core.uf2"


def _run(command: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def build_uf2() -> Path:
    """Clones CoreMark and pico-sdk, applies the patch, builds, and caches the UF2."""
    missing = [tool for tool in ("git", "cmake", "arm-none-eabi-gcc") if shutil.which(tool) is None]
    if missing:
        sys.exit(f"building the dual-core CoreMark needs {', '.join(missing)} on PATH (or pass --image)")
    with tempfile.TemporaryDirectory(prefix="coremark-dual-") as work_name:
        work = Path(work_name)
        sdk, source, out = work / "pico-sdk", work / "CoreMark-RP2040", work / "out"
        _run(["git", "clone", "--depth", "1", "--branch", PICO_SDK_TAG, PICO_SDK_URL, str(sdk)])
        _run(["git", "submodule", "update", "--init", "--depth", "1", "lib/tinyusb"], cwd=sdk)
        _run(["git", "clone", "--depth", "1", "--branch", COREMARK_TAG, COREMARK_URL, str(source)])
        _run(["git", "apply", str(PATCH)], cwd=source)
        out.mkdir()
        env = {
            **os.environ,
            "PICO_SDK_PATH": str(sdk),
        }  # (CoreMark's CMakeLists reads it from the environment, not from -D)
        _run(["cmake", "-DCMAKE_BUILD_TYPE=Release", str(source)], cwd=out, env=env)
        _run(["cmake", "--build", ".", "--parallel"], cwd=out)
        CACHED.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out / "Coremark-RP2040.uf2", CACHED)
    return CACHED


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--image" not in args:
        args = ["--image", str(CACHED if CACHED.exists() else build_uf2()), *args]
    if "--expect-text" not in args:
        args = ["--expect-regex", "--expect-text", RESULT_REGEX, *args]
    _feed_enter_to_stdin()
    main(["micropython", *args])
