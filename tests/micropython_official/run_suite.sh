#!/usr/bin/env bash
# Runs MicroPython's own test files (tests/ports/rp2, tests/extmod/machine_*) against the emulated board, one file at a time, through the console's pty.
#
# It reports: every file ends as PASS / FAIL / SKIP / HANG / ERROR in $OUT (a markdown table). The script exits 0 - except with STRICT=1 (a group that gates the build, see
# .github/workflows/ci-micropython-official.yml), where any FAIL, ERROR or HANG makes it exit 1; a SKIP never does. A file that hangs is killed after
# $PER_TEST_TIMEOUT seconds and the emulator is restarted for the next one. Why the failures happen is for a human to sort (hardware-only, a missing model, or a bug): see
# docs/records/0098-datasheet-conformance-audit.md.
#
# usage: run_suite.sh <micropython checkout> <image tag> <output .md> [test glob relative to tests/ ...]
#        (default globs: ports/rp2/*.py extmod/machine_*.py; GROUP_TITLE names the group in the summary; STRICT=1 gates, see above)
#        CORE1=1 runs the files that use `_thread` (skipped otherwise: see below).
#        BOARD_SPEC=file.py:ATTR boots the emulator through that board (the extmod_hardware group needs GPIO0 wired to GPIO1: tests/micropython_official/wired_board.py:BOARD).
#        RETRY_HANG=0 turns off the one retry a hanging file gets (default 1): see the loop.
#        EXCLUDE="io/file1.py ..." marks tests that cannot pass on a board for a reason that is not the emulator's as SKIP (platform), and never runs them.
#        PUSH="io/data: import/*:" copies those paths (relative to tests/, "src:destination on the board") onto the board after every emulator start: the tests that open
#        files ("data/file1") or import helper modules expect them next to the test, as they are on the host.
set -u
MP_DIR=$1
TAG=$2
OUT=$3
shift 3
GLOBS=("$@")
[ ${#GLOBS[@]} -eq 0 ] && GLOBS=("ports/rp2/*.py" "extmod/machine_*.py")
TITLE=${GROUP_TITLE:-}
STRICT=${STRICT:-0}
PER_TEST_TIMEOUT=${PER_TEST_TIMEOUT:-45}
WORK=$(mktemp -d)
EMU_LOG=$WORK/emu.log
EMU_PID=

start_emulator() {
  # its own process group, so stop_emulator can kill everything it started; run from the environment's bin/ (not `uv run`): after the `cd` into MicroPython's tests/ a restart
  # (HANG) would otherwise run `uv run` in a directory that is not this project and fail to find `rp2040py`, turning every later file into an ERROR
  setsid "$VIRTUAL_ENV/bin/rp2040py" micropython --image "$TAG" ${BOARD_SPEC:+--board-spec "$BOARD_SPEC"} --pty >"$EMU_LOG" 2>&1 &
  EMU_PID=$!
  for _ in $(seq 60); do grep -q "PTY REPL listening" "$EMU_LOG" && break; sleep 1; done
  DEV=$(grep -o "/dev/pts/[0-9]*" "$EMU_LOG" | head -1)
  # mip downloads on the host and copies to the device, so the board needs no network
  timeout 180 "$VIRTUAL_ENV/bin/mpremote" connect "$DEV" mip install unittest >/dev/null 2>&1 || true
  for pair in ${PUSH:-}; do
    for src in "$MP_DIR"/tests/${pair%%:*}; do  # (the source may be a glob: import/*)
      timeout 180 "$VIRTUAL_ENV/bin/mpremote" connect "$DEV" fs cp -r "$src" ":${pair#*:}" >/dev/null 2>&1 || true
    done
  done
}

# The emulator's idle time is not paced: while the board waits for the host, simulated time runs far ahead of the wall clock (years per few seconds), and the RTC with it. A persistent
# board therefore reaches a date past 2136 after enough files, where MicroPython's 32-bit seconds-since-2000 wrap and time.time() / os.stat() times disagree with time_ns() by 2**32 s
# (extmod/time_time_ns.py, extmod/vfs_lfs_mtime.py). A real board's RTC is set by whoever runs the tests; so is this one, before each file.
set_clock() {
  timeout 60 "$VIRTUAL_ENV/bin/mpremote" connect "$DEV" exec "import machine; machine.RTC().datetime((2024, 1, 1, 0, 0, 0, 0, 0))" >/dev/null 2>&1 || true
}

stop_emulator() {
  [ -n "$EMU_PID" ] || return 0
  kill -TERM -- "-$EMU_PID" 2>/dev/null  # the whole group (setsid made the wrapper its leader)
  for _ in 1 2 3 4 5; do kill -0 "$EMU_PID" 2>/dev/null || break; sleep 1; done
  kill -KILL -- "-$EMU_PID" 2>/dev/null  # an emulator that ignores SIGTERM must not hang the run
  wait "$EMU_PID" 2>/dev/null
  true
}

ROWS=$WORK/rows.md      # every file
BAD=$WORK/bad.md        # only the files that did not pass: this is what the run summary shows
RETRIED=$WORK/retried.md  # the files that hung once and then ran
: >"$ROWS"
: >"$BAD"
: >"$RETRIED"

start_emulator
cd "$MP_DIR/tests" || exit 0
pass=0 fail=0 skip=0 hang=0
shopt -s nullglob
for t in $(for g in "${GLOBS[@]}"; do ls $g 2>/dev/null; done | sort); do
  log=$WORK/$(echo "$t" | tr / _).log
  # `_thread` starts the second core (multicore_launch_core1 waits for the bootrom FIFO handshake). The emulator has core 1 now (record 0104), but the groups that gate the build were
  # decided without it: a file that uses `_thread` is skipped there unless CORE1=1 (the report-only `thread/` group sets it).
  case " ${EXCLUDE:-} " in
    *" $t "*)
      echo "| $t | :fast_forward: SKIP (platform, see the workflow) |" >>"$ROWS"
      echo "| $t | :fast_forward: SKIP (platform, see the workflow) |" >>"$BAD"
      echo "$t: SKIP (platform)"; skip=$((skip + 1)); continue ;;
  esac
  if [ "${CORE1:-0}" != 1 ] && grep -q "_thread" "$t"; then
    echo "| $t | :fast_forward: SKIP (needs core 1) |" >>"$ROWS"
    echo "| $t | :fast_forward: SKIP (needs core 1) |" >>"$BAD"
    echo "$t: SKIP (needs core 1)"; skip=$((skip + 1)); continue
  fi
  set_clock
  timeout "$PER_TEST_TIMEOUT" "$VIRTUAL_ENV/bin/python" run-tests.py -t "$DEV" -r "$WORK/res" "$t" >"$log" 2>&1
  rc=$?
  if [ $rc = 124 ] && [ "${RETRY_HANG:-1}" = 1 ]; then
    # A hang on a shared CI runner is not always the test's: one (misc/non_compliant_lexer.py, 6 s on a workstation, passed on the re-run of the same commit) stopped a gating group. The emulator is
    # restarted and the file run once more; a file that hangs twice is a HANG, one that passes the second time is reported as retried so the flake stays visible.
    echo "$t: HANG, retrying once on a fresh emulator"
    stop_emulator; start_emulator; set_clock
    timeout "$PER_TEST_TIMEOUT" "$VIRTUAL_ENV/bin/python" run-tests.py -t "$DEV" -r "$WORK/res" "$t" >"$log" 2>&1
    rc=$?
    [ $rc != 124 ] && echo "| $t | hung once, then ran |" >>"$RETRIED"
  fi
  if [ $rc = 124 ]; then
    r=":hourglass: HANG"; hang=$((hang + 1)); stop_emulator; start_emulator
  elif grep -q "^pass " "$log"; then r=":white_check_mark: PASS"; pass=$((pass + 1))
  elif grep -q "^FAIL " "$log"; then r=":x: FAIL"; fail=$((fail + 1))
  elif grep -qi "^skip" "$log"; then r=":fast_forward: SKIP"; skip=$((skip + 1))
  else r=":warning: ERROR (see the job log)"; fail=$((fail + 1)); fi
  echo "| $t | $r |" >>"$ROWS"
  case "$r" in *PASS*) ;; *) echo "| $t | $r |" >>"$BAD" ;; esac
  echo "$t: $r"
done
stop_emulator
{
  echo "## MicroPython official tests on the emulated RP2040 ($TAG)${TITLE:+ - $TITLE}"
  echo
  echo "**:white_check_mark: $pass passed, :x: $fail failed, :fast_forward: $skip skipped, :hourglass: $hang hung** - $([ "$STRICT" = 1 ] && echo "this group gates the build: a failure or a hang fails the job." || echo "report only, nothing here gates the build.")"
  echo
  if [ -s "$BAD" ]; then
    echo "Not passing:"
    echo
    echo "| test | result |"
    echo "|---|---|"
    cat "$BAD"
  else
    echo "Every file passed."
  fi
  if [ -s "$RETRIED" ]; then
    echo
    echo "Hung once and ran on a fresh emulator (counted by how they ran; a flake to watch, not a pass to trust blindly):"
    echo
    echo "| test | note |"
    echo "|---|---|"
    cat "$RETRIED"
  fi
  echo
  echo "Why the failures happen: see \`docs/records/0098-datasheet-conformance-audit.md\` (section on MicroPython's own test suite). The full per-file list is in the job's artifact."
} >"$OUT"
{
  echo "## All files"
  echo
  echo "| test | result |"
  echo "|---|---|"
  cat "$ROWS"
} >"${OUT%.md}-full.md"
cat "$OUT"
# straight into the run's summary page as well, so it shows even if a later step fails
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && cat "$OUT" >>"$GITHUB_STEP_SUMMARY"
[ "$STRICT" = 1 ] && [ $((fail + hang)) -gt 0 ] && exit 1
exit 0
