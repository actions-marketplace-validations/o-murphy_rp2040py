"""The tests that need littlefs-python skip themselves on PyPy: the package publishes no wheels for it, so the pure-Python build is tested there without it (and with it built from source it
does not read an image back correctly)."""

import sys
from types import ModuleType

import pytest

__all__ = ("importorskip_littlefs",)


def importorskip_littlefs() -> ModuleType:
    if sys.implementation.name == "pypy":
        pytest.skip(
            "littlefs-python has no wheels for PyPy: the tests that need it run on CPython", allow_module_level=True
        )
    return pytest.importorskip("littlefs", reason="mklittlefs needs the optional 'fs' extra (littlefs-python)")
