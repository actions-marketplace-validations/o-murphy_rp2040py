#!/usr/bin/env python3
"""CoreMark on the emulated RP2040: downloads the single-core UF2 into the firmware cache, runs it
through the USB CDC console and exits on the result line.

Usage: python demo/coremark_run.py [--image firmware.uf2 | URL] [extra `rp2040py micropython` flags]

Thin wrapper around `rp2040py micropython`. The image is fetched and cached in
~/.cache/rp2040py exactly as `--image <url>` is for any other firmware, so it is downloaded once.

Two things this script does that a plain `rp2040py micropython --image ...` would not:

- The firmware prints "Press any key to continue" and waits on the USB CDC console (`getchar()`),
  so an Enter is fed to the console's stdin once the device has had time to enumerate.
- It stops on the `Iterations/Sec : <n>` result line. A bare `--expect-text "Iterations/Sec"` would
  match the banner's own sentence ("Iterations/Sec is the main benchmark result...") and stop
  before the benchmark has run.

The default image is protik09/CoreMark-RP2040 **v1.0.0**, the single-core build. Later releases
(v1.0.4 .. v1.0.7 as of 2026-10) run the iterations on core 1 as well and hang here: the emulator
has no second core yet (docs/records/0096-cpp-mcu-core.md). Note the cache is keyed by the asset's
file name, which every release of that repository shares (`Coremark-RP2040.uf2`) - delete the
cached file before switching to another release's URL.

The run takes a few minutes of wall time: the benchmark needs ~12 s of simulated chip time and the
emulator runs a busy core at roughly 0.1-0.2x real time.
"""

import os
import sys
import threading
import time

from rp2040py.cli import main

IMAGE_URL = "https://github.com/protik09/CoreMark-RP2040/releases/download/v1.0.0/Coremark-RP2040.uf2"
RESULT_REGEX = r"Iterations/Sec\s*:\s*[0-9]"
_ENTER_DELAY_SECONDS = 3.0


def _feed_enter_to_stdin() -> None:
    """Replaces `sys.stdin` with a pipe that gets one Enter after `_ENTER_DELAY_SECONDS` and is then
    left open (closing it would read as end of input). The CLI reads it as a non-tty stream."""
    read_fd, write_fd = os.pipe()
    sys.stdin = os.fdopen(read_fd, "r")

    def feed() -> None:
        time.sleep(_ENTER_DELAY_SECONDS)
        os.write(write_fd, b"\n")

    threading.Thread(target=feed, daemon=True).start()


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--image" not in args:
        args = ["--image", IMAGE_URL, *args]
    if "--expect-text" not in args:
        args = ["--expect-regex", "--expect-text", RESULT_REGEX, *args]
    _feed_enter_to_stdin()
    main(["micropython", *args])
