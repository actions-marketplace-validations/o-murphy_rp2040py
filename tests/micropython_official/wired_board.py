"""The board `tests/target_wiring/rp2.py` of MicroPython describes - a Pico with GPIO0 wired to GPIO1 - for the `extmod_hardware` group of the official suite
(`run_suite.sh` takes `BOARD_SPEC=tests/micropython_official/wired_board.py:BOARD`)."""

from dataclasses import replace

from rp2040py.boards import BOARDS
from rp2040py.external.jumper import Jumper

BOARD = replace(BOARDS["pico"], extras=(*BOARDS["pico"].extras, lambda: Jumper(0, 1)))
