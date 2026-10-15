"""The mutant generator of scripts/audit/cpp_mutants.py: which lines it changes and how (the full run over a block is the script's own job, not a per-push test)."""

import importlib.util
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "audit" / "cpp_mutants.py"
if not SCRIPT.exists():  # cibuildwheel's Android and Pyodide runs copy only tests/ next to the wheel
    pytest.skip("scripts/ is not part of this test environment", allow_module_level=True)
SPEC = importlib.util.spec_from_file_location("cpp_mutants", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
cpp_mutants = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cpp_mutants)

SOURCE = [
    "constexpr int kBefore = 1 | 2;",
    "class Block {",
    "public:",
    "    bool ready(int a, int b) {",
    "        if (a == b && a < 4) return true;  // a comment with == in it",
    "        count_ = a + b;",
    "        return false;",
    "    }",
    "    int count_ = 0;",
    "};",
    "",
]


def test_only_lines_from_the_first_class_on_are_mutated():
    assert all(index >= 1 for index, _rule, _text in cpp_mutants.mutants(SOURCE))


def test_a_line_of_logic_gets_one_mutant_per_rule_and_occurrence():
    found = [(rule, text.strip()) for index, rule, text in cpp_mutants.mutants(SOURCE) if index == 4]
    assert ("== -> !=", "if (a != b && a < 4) return true;  // a comment with == in it") in found
    assert ("&& -> ||", "if (a == b || a < 4) return true;  // a comment with == in it") in found
    assert (" <  ->  <= ", "if (a == b && a <= 4) return true;  // a comment with == in it") in found
    assert not any("//" in text and "!=" in text.split("//")[1] for _rule, text in found)  # comments are not mutated


def test_statements_can_be_deleted_but_a_plain_return_of_a_constant_is_kept():
    deletions = {index for index, rule, _text in cpp_mutants.mutants(SOURCE) if rule == "delete"}
    assert 5 in deletions  # count_ = a + b;
    assert 6 not in deletions  # return false;


def test_the_key_names_the_line_text_and_the_rule_not_its_number():
    assert cpp_mutants.key(SOURCE, 5, "+ -> -") == "count_ = a + b; | + -> -"
