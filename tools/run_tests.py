"""
Builds and uploads the firmware, waits for the board, and runs both tests.

    python tools/run_tests.py [address] [options]

    address        The board, for example http://172.16.100.185. The default
                   is http://esp32.local. A name is looked up only once: the
                   tests then run against the IP address the board reports.

    --no-upload    Test the firmware already on the board.
    --uploadfs     Also upload the filesystem image (the data folder) first.
                   Only needed after changing the database or the stylesheet.
    --env NAME     The PlatformIO environment to build (default Release).
    --api-only     Run only tools/test_api.py.
    --page-only    Run only tools/test_page.py.

The steps stop at the first failure. The exit code is 0 only when every step
succeeded and every check passed, so the script can be used by other tools.

Only the standard library is used. PlatformIO is found on the PATH, or in its
usual place in the user's home folder.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# How long the board may take to come back after an upload: it restarts,
# joins the WiFi network (up to 20 seconds, WIFI_CONNECT_TIMEOUT_MS), and
# opens the database.
BOOT_TIMEOUT = 60


def find_pio():
    """Returns the path of the PlatformIO command line tool."""
    found = shutil.which("pio")
    if found:
        return found
    home = os.path.expanduser("~")
    for candidate in (os.path.join(home, ".platformio", "penv", "Scripts", "pio.exe"),
                      os.path.join(home, ".platformio", "penv", "bin", "pio")):
        if os.path.exists(candidate):
            return candidate
    sys.exit("PlatformIO was not found. Install it or put pio on the PATH.")


def step(title):
    print("\n=== %s %s" % (title, "=" * max(0, 70 - len(title))), flush=True)


def run(command):
    """Runs a command in the project folder, its output going straight to the console. Returns True on success."""
    return subprocess.run(command, cwd=PROJECT).returncode == 0


def board_status(base):
    """
    The board's /api/status, or None while it does not answer. Something
    else answering at that address, with an error or a different document,
    counts as no answer.
    """
    try:
        with urllib.request.urlopen(base + "/api/status", timeout=3) as response:
            status = json.loads(response.read())
    except Exception:
        return None
    return status if isinstance(status, dict) and "uptime" in status else None


def wait_for_board(base, since):
    """
    Waits until the board answers and has started after the given moment.

    "Started after" is judged by its uptime: a board that answers but has been
    running for longer than the time since the upload is still the old
    firmware, about to restart, and is not tested.
    """
    deadline = time.monotonic() + BOOT_TIMEOUT
    while time.monotonic() < deadline:
        status = board_status(base)
        if status and status.get("uptime", 1e9) <= time.monotonic() - since + 2:
            return status
        time.sleep(1)
    return None


def main():
    parser = argparse.ArgumentParser(description="Upload the firmware and run both tests against the board.")
    parser.add_argument("address", nargs="?", default="http://esp32.local")
    parser.add_argument("--no-upload", action="store_true")
    parser.add_argument("--uploadfs", action="store_true")
    parser.add_argument("--env", default="Release")
    parser.add_argument("--api-only", action="store_true")
    parser.add_argument("--page-only", action="store_true")
    options = parser.parse_args()

    base = options.address.rstrip("/")
    if "://" not in base:
        base = "http://" + base

    started = time.monotonic()
    results = []

    if not options.no_upload:
        pio = find_pio()

        if options.uploadfs:
            step("Uploading the filesystem image")
            if not run([pio, "run", "-e", options.env, "-t", "uploadfs"]):
                sys.exit("Uploading the filesystem failed.")

        step("Building and uploading the firmware (%s)" % options.env)
        if not run([pio, "run", "-e", options.env, "-t", "upload"]):
            sys.exit("Building or uploading the firmware failed.")

    step("Waiting for the board at %s" % base)
    uploaded = time.monotonic()
    status = wait_for_board(base, uploaded if not options.no_upload else uploaded - 1e9)
    if not status:
        sys.exit("The board did not answer within %d seconds." % BOOT_TIMEOUT)
    print("Up after %.0f s: firmware built %s, uptime %s s, address %s"
          % (time.monotonic() - uploaded, status.get("build"), status.get("uptime"), status.get("ip")))

    if status.get("fs_mounted") is False or status.get("db_open") is False:
        sys.exit("The board has no usable database (fs_mounted %s, db_open %s). Run again with --uploadfs."
                 % (status.get("fs_mounted"), status.get("db_open")))
    if status.get("crash_dump"):
        print("NOTE: a crash dump is waiting (task %s at %s); see 'Reading a crash' in docs/how-it-works.md."
              % (status.get("crash_task"), status.get("crash_pc")))

    # The tests make hundreds of requests. Given a name such as esp32.local,
    # each would look the name up again, which on Windows takes up to a
    # second and now and then fails. The board reports its own IP address,
    # so the name is looked up only once, here, and the tests use the address.
    ip = status.get("ip")
    host = base.split("://", 1)[1].split("/", 1)[0].split(":", 1)[0]
    if ip and host != ip:
        base = base.replace(host, ip, 1)
        print("Testing at %s (the address %s reports for itself)" % (base, host))

    tests = []
    if not options.page_only:
        tests.append(("Address test", "test_api.py"))
    if not options.api_only:
        tests.append(("Browser test", "test_page.py"))

    for title, script in tests:
        step(title)
        passed = run([sys.executable, os.path.join(PROJECT, "tools", script), base])
        results.append((title, passed))

    step("Summary")
    for title, passed in results:
        print("%-14s %s" % (title, "passed" if passed else "FAILED"))
    print("Took %.0f s" % (time.monotonic() - started))

    sys.exit(0 if all(passed for _, passed in results) else 1)


if __name__ == "__main__":
    main()
