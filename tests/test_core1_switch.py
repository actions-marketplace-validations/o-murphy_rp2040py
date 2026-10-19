"""The second core can be switched off (docs/records/0104): `RP2040PY_CORE1=0` in the environment, which the CLI's `--no-core1` sets, gives back the old stub - the inter-core FIFO registers warn and read
as 0xFFFFFFFF and nothing launches core 1 - in both builds. It is on by default."""

import pytest
from utils.chip_pair import Native, PurePython, make_chip

from rp2040py import cli

SIO_FIFO_ST = 0xD0000050


@pytest.mark.parametrize("cls", [PurePython, Native], ids=["pure", "native"])
def test_core_1_is_on_by_default(cls, monkeypatch):
    monkeypatch.delenv("RP2040PY_CORE1", raising=False)
    assert make_chip(cls).read_uint32(SIO_FIFO_ST) == 0x2  # RDY: the real FIFO, empty


@pytest.mark.parametrize("cls", [PurePython, Native], ids=["pure", "native"])
@pytest.mark.parametrize("value", ["0", "off", "False", "no"])
def test_the_environment_variable_switches_it_off(cls, value, monkeypatch):
    monkeypatch.setenv("RP2040PY_CORE1", value)
    assert make_chip(cls).read_uint32(SIO_FIFO_ST) == 0xFFFFFFFF  # the old stub


@pytest.mark.parametrize("command", ["run", "micropython", "kaluma", "bench"])
def test_no_core1_flag_of_the_cli_sets_the_environment_variable(command, monkeypatch):
    monkeypatch.setenv("RP2040PY_CORE1", "1")  # (registers the variable, so the CLI's change is undone with the test)
    seen = []
    monkeypatch.setattr(cli, f"_cmd_{command}", lambda args: seen.append(args))
    cli.main([command, "--no-core1"])
    assert seen and seen[0].no_core1 is True
    assert cli.os.environ["RP2040PY_CORE1"] == "0"
