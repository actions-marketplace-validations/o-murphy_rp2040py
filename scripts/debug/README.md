# Debugging tools

Small scripts that were written to find one specific problem and are worth keeping for the next one. None of them is run by CI.

| script | what it is for |
|---|---|
| `usb_trace.py` | every access of a firmware to the USB controller, with the time, and the last accesses when it hangs (the pico-sdk 1.2.0-1.4.0 enumeration, record 0098) |
| `build_pico_hello_usb.sh` | builds pico-examples' `hello_usb` for one pico-sdk version (needs an ARM toolchain), to reproduce a "Test Pi Pico SDK" failure locally |
| `live_boots.sh` | MicroPython / CircuitPython / Kaluma boots with timings |
| `gdb_clock_alarms.py` | from gdb, the alarm list of a spinning native emulator (record 0100) |
| `../audit/effect_probe.py` | does writing a register change anything (behind `register_status.py`'s stub list) |
| `../audit/cpp_mutants.py` | mutation testing of a C++ core block against its checks |
| `../../tests/micropython_official/run_suite.sh` | MicroPython's own test suite against the emulator |

Hard-won habits (each cost an hour): never `pkill -f` / `pgrep -f` with a string that also appears in your own command line (it kills your shell); to give a background job a stdin use `< <(sleep N)`;
run Python with `-I` from a directory that holds a file named like a standard-library module (`inspect.py` shadowed the real one).
