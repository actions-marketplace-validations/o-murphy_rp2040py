"""The reset cascade, pure-Python chip against native chip, in lockstep (docs/records/0096-cpp-mcu-core.md, step 3b; the oracle is tests/utils/chip_diff.py)."""

import pytest

from tests.utils import chip_diff
from tests.utils.chip_pair import Native, PurePython, make_chip

SEEDS = range(40)


def _both():
    return make_chip(PurePython), make_chip(Native)


@pytest.mark.parametrize("seed", SEEDS)
def test_the_two_cascades_agree(seed):
    pure, native = _both()
    assert chip_diff.run(pure, native, chip_diff.make_ops(seed)) is None


@pytest.mark.parametrize("name", sorted(chip_diff.MUTANTS))
def test_every_mutant_of_the_reference_is_seen(name):
    for seed in SEEDS:
        pure, native = _both()
        chip_diff.MUTANTS[name](pure)
        if chip_diff.run(pure, native, chip_diff.make_ops(seed)) is not None:
            return
    pytest.fail(f"the oracle did not see the mutant {name!r} in {len(SEEDS)} runs")
