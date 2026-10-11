import gc
import threading

import pytest
from utils.is32bit import IS32BIT

from rp2040py.rp2040 import RP2040

rp2040_semaphore = threading.Semaphore(4 if IS32BIT else 8)


@pytest.fixture
def rp2040_factory():
    created_instances = []

    def _factory(*args, **kwargs):
        rp2040_semaphore.acquire()
        try:
            rp = RP2040(*args, **kwargs)
            created_instances.append(rp)
            return rp
        except Exception:
            rp2040_semaphore.release()
            raise

    yield _factory

    for _ in created_instances:
        rp2040_semaphore.release()

    created_instances.clear()
    gc.collect()


@pytest.fixture(autouse=True)
def _collect_between_tests_on_a_32_bit_build():
    """A chip is ~16 MB of flash and lives in reference cycles, so it is freed by the cyclic collector, not when the last name goes. On a 64-bit build the few that wait for the next automatic
    collection cost nothing; in a 32-bit address space (CI's ARMv7 and Windows x86 wheel tests) a test that builds hundreds of chips directly - the lockstep oracles do, bypassing
    `rp2040_factory` and its limit - fragments it until a 16 MB allocation fails with a MemoryError. Collecting after each test keeps the peak at one test's chips."""
    yield
    if IS32BIT:
        gc.collect()


# PyPy only: littlefs-python's LittleFS/file C objects are finalised out of order when the interpreter shuts down ("lfs_file_close: Assertion `lfs_mlist_isopen(...)' failed", SIGABRT)
# after any test has built an image in-process - an upstream limitation the CLI already works around with `os._exit()` (cli/__init__.py, `_cmd_mklittlefs`). A test run that has
# passed must not end in a crash, so on PyPy the session ends by leaving through `os._exit()` with pytest's own exit status once everything has been reported.
def pytest_sessionfinish(session, exitstatus):
    session.config._rp2040py_exitstatus = int(exitstatus)


def pytest_unconfigure(config):
    import os
    import sys

    if sys.implementation.name == "pypy":
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(getattr(config, "_rp2040py_exitstatus", 0))
