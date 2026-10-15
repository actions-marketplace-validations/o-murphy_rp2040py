#!/usr/bin/env bash
# Live boots of the firmwares CI boots, one after another, with the wall time of each: MicroPython (REPL over the USB CDC), CircuitPython, Kaluma. The console reader needs stdin to be a pipe
# or a terminal, hence `< <(sleep N)`. Run it before pushing a change to a peripheral the firmwares touch; the pico-sdk examples need scripts/debug/build_pico_hello_usb.sh.
cd "$(dirname "$0")/../.."
R=.venv/bin/rp2040py
run() {
  local start end
  start=$(date +%s)
  "$@" < <(sleep 250) 2>&1 | tail -3
  end=$(date +%s)
  echo "  rc=${PIPESTATUS[0]}  $((end - start)) s"
}
echo "== MicroPython 1.29.0 (REPL over the CDC, main.py from littlefs)"
$R mklittlefs -o /tmp/live-lfs.img tests/micropython/main.py --main main.py > /dev/null
run timeout 240 $R micropython --image 1.29.0 --littlefs /tmp/live-lfs.img --expect-text "Hello, MicroPython!"
echo "== CircuitPython 10.2.1"
run timeout 240 $R micropython --circuitpython --image 10.2.1 tests/circuitpython/main-reset-cause.py
echo "== Kaluma 1.2.1"
run timeout 240 $R kaluma --image 1.2.1 tests/kaluma/index.js --expect-text "Hello, Kaluma!"
