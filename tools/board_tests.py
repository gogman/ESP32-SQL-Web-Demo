"""
The board tests in a form pytest, and so VS Code's Testing panel, can run.

Each test runs one of the existing test scripts against the board and passes
when the script does. The scripts' own output, with every "ok" and "FAIL"
line, appears in the test's output, and a failure lists the checks that
failed.

The board must already be running the firmware to be tested: nothing here
builds or uploads. Use tools/run_tests.py, or the VS Code task, for that.

The board's address comes from the environment variable BOARD_URL, default
http://esp32-sql.local. In VS Code it can be set in a file named .env in the
project folder, for example:

    BOARD_URL=http://172.16.100.185

From a terminal:

    .venv/Scripts/python -m pytest              (uses pytest.ini)
"""

import os
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_tests import PROJECT, board_status  # noqa: E402

# The longest a test script may run before it is considered stuck, in seconds.
SCRIPT_TIMEOUT = 600


@pytest.fixture(scope="session")
def board():
    """
    The board's status and the address to test it at.

    A name such as esp32-sql.local is looked up once, here: the tests then use
    the IP address the board reports, as tools/run_tests.py does.
    """
    base = os.environ.get("BOARD_URL", "http://esp32-sql.local").rstrip("/")
    if "://" not in base:
        base = "http://" + base

    # A few tries: looking up a name such as esp32-sql.local fails now and then.
    status = None
    for attempt in range(3):
        status = board_status(base)
        if status:
            break
        time.sleep(1)
    if not status:
        pytest.fail("The board does not answer at %s. Set BOARD_URL to its address." % base)

    if status.get("ip"):
        base = "http://" + status["ip"]
    return base, status


def run_script(script, base):
    """Runs one test script against the board and fails with the checks that failed."""
    result = subprocess.run(
        [sys.executable, os.path.join(PROJECT, "tools", script), base],
        cwd=PROJECT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        env=dict(os.environ, PYTHONIOENCODING="utf-8"), timeout=SCRIPT_TIMEOUT)

    # Shown by pytest with the test, and in VS Code under the test's output.
    print(result.stdout)
    print(result.stderr, file=sys.stderr)

    failures = [line for line in result.stdout.splitlines() if line.startswith("FAIL")]
    if result.returncode != 0:
        detail = "\n".join(failures) or (result.stderr.strip().splitlines() or ["(no output)"])[-1]
        pytest.fail("%s failed:\n%s" % (script, detail), pytrace=False)


def test_board_is_ready(board):
    """The board answers, has its filesystem and database, and has not crashed."""
    _, status = board
    assert status.get("fs_mounted") is True, "the filesystem is not mounted; upload it with pio run -t uploadfs"
    assert status.get("db_open") is True, "the database is not open: %s" % status.get("db_error")
    assert not status.get("crash_dump"), ("a crash dump is waiting (task %s at %s); see 'Reading a crash' in docs/how-it-works.md"
                                          % (status.get("crash_task"), status.get("crash_pc")))


def test_addresses(board):
    """tools/test_api.py: every address of the board, compared with the database file."""
    run_script("test_api.py", board[0])


def test_page_in_browser(board):
    """tools/test_page.py: the page in a real browser."""
    run_script("test_page.py", board[0])


def test_setup_page(board):
    """tools/test_setup.py: the setup page and the settings, including a save and two restarts."""
    run_script("test_setup.py", board[0])
