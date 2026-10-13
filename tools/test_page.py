"""Checks the application page in a real browser.

Usage (from the project folder):
    python tools/test_page.py [http://address-of-the-board] [path-to-browser]

The default address is http://esp32.local. Passing the board's IP address instead
avoids a name lookup of about a second on Windows for every page that is opened.

tools/test_api.py checks the addresses the page calls. This script checks the page
itself: it opens it in Microsoft Edge or Google Chrome, running without a window,
and looks at what a person would see. It searches, pages forward and back, sorts
columns and uses the browser's back button, and compares the table on screen with
data/babyname.db.

Nothing has to be installed. The browser is driven through its built-in remote
debugging interface (the Chrome DevTools Protocol), spoken here with only the
Python standard library.

The exit code is 0 when every check passed and 1 otherwise.
"""

import base64
import json
import os
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

# The project folder: the parent of the folder this script is in.
PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where the browsers are normally installed on Windows. The first one that exists is used.
BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
]

# The same query the firmware runs, without paging; see tools/test_api.py.
RANGE_SQL = ("Select year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state "
             "from gendered_names where name between ? and ? order by name, year, state")

# How long to wait for something to appear on the page before giving up, in seconds.
TIMEOUT = 20

# The number of checks that did not pass.
failures = 0


def check(ok, what):
    """Prints one result line, "ok" or "FAIL" followed by a description, and counts failures."""
    global failures
    if not ok:
        failures += 1
    print("%-4s %s" % ("ok" if ok else "FAIL", what))


class WebSocket:
    """A minimal WebSocket client: just enough to exchange text messages with the browser.

    The browser's debugging interface speaks WebSocket, and Python's standard library
    has no client for it. The protocol is simple, though: an HTTP request asks the
    server to switch protocols, and after that both sides send "frames", each a small
    header followed by the data.
    """

    def __init__(self, url):
        # url looks like ws://127.0.0.1:9222/devtools/page/<id>
        address, _, path = url[len("ws://"):].partition("/")
        host, _, port = address.partition(":")
        self.socket = socket.create_connection((host, int(port)), timeout=60)

        # The opening handshake. The key is a random value the server must echo back
        # in a transformed form; this client does not verify it, since it only ever
        # talks to a browser it started itself on this computer.
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((
            "GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, address, key)).encode())

        # Read the server's reply up to the empty line that ends it.
        reply = b""
        while b"\r\n\r\n" not in reply:
            reply += self.socket.recv(1)
        if b" 101 " not in reply.split(b"\r\n")[0]:
            raise RuntimeError("The browser refused the debugging connection: %r" % reply)

    def _read(self, count):
        """Reads exactly count bytes; recv() alone may return fewer."""
        data = b""
        while len(data) < count:
            part = self.socket.recv(count - len(data))
            if not part:
                raise RuntimeError("The browser closed the debugging connection")
            data += part
        return data

    def send(self, text):
        """Sends one text message."""
        payload = text.encode()

        # First byte: 0x80 marks the final (here: only) frame of the message, 0x01
        # says it is text. Second byte: 0x80 says the data is masked, plus the
        # length. Lengths that do not fit in 7 bits use 126 or 127 as a marker,
        # followed by the real length in 2 or 8 bytes.
        header = bytes([0x81])
        if len(payload) < 126:
            header += bytes([0x80 | len(payload)])
        elif len(payload) < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", len(payload))

        # A client must "mask" what it sends: every byte is combined with one of
        # four random bytes, which are sent along. It is a requirement of the
        # protocol, not a security measure for this script.
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        self.socket.sendall(header + mask + masked)

    def receive(self):
        """Returns the next text message, joining it together if it arrives in several frames."""
        message = b""
        while True:
            first, second = self._read(2)
            opcode, final = first & 0x0F, first & 0x80
            length = second & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            data = self._read(length)

            if opcode == 0x8:
                raise RuntimeError("The browser closed the debugging connection")
            if opcode == 0x9:
                # A ping only checks that the connection is alive. It needs no
                # answer for the short life of this script.
                continue

            # Opcode 1 starts a text message and opcode 0 continues one.
            message += data
            if final:
                return message.decode()

    def close(self):
        self.socket.close()


class Browser:
    """A browser without a window, opened on one page and controlled from here."""

    def __init__(self, executable):
        # A throwaway profile folder keeps the test away from the user's own
        # browser profile, and guarantees an empty cache at the start.
        self.profile = tempfile.mkdtemp(prefix="sqlite_web_test_")

        # Ask the system for a free port for the debugging interface: bind to port
        # 0, see which port was given, and release it again for the browser.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]

        self.process = subprocess.Popen([
            executable,
            "--headless=new",                       # no window
            "--remote-debugging-port=%d" % port,    # the interface this script talks to
            "--remote-allow-origins=*",             # accept the connection made below
            "--user-data-dir=" + self.profile,
            "--no-first-run",
            "--disable-gpu",
            "about:blank",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # The browser lists its open pages at /json/list once it is ready. Each
        # entry carries the WebSocket address for controlling that page.
        deadline = time.time() + TIMEOUT
        target = None
        while target is None:
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % port, timeout=2) as reply:
                    pages = [entry for entry in json.loads(reply.read()) if entry.get("type") == "page"]
                    if pages:
                        target = pages[0]["webSocketDebuggerUrl"]
            except OSError:
                if time.time() > deadline:
                    raise RuntimeError("The browser did not start its debugging interface")
                time.sleep(0.2)

        self.connection = WebSocket(target)
        self.next_id = 0

    def call(self, method, **params):
        """Runs one debugging command and returns its result.

        Every command is sent with a number, and the browser's answer carries the
        same number. In between, the browser may send notifications about things
        happening on the page; they are not needed here and are skipped.
        """
        self.next_id += 1
        self.connection.send(json.dumps({"id": self.next_id, "method": method, "params": params}))
        while True:
            message = json.loads(self.connection.receive())
            if message.get("id") == self.next_id:
                if "error" in message:
                    raise RuntimeError("%s failed: %s" % (method, message["error"]))
                return message["result"]

    def run(self, script):
        """Runs JavaScript in the page and returns its value.

        If the script produces a promise, the browser waits for it and returns what
        it resolves to; that is how wait_for() below can wait inside the page.
        """
        result = self.call("Runtime.evaluate", expression=script, awaitPromise=True, returnByValue=True)
        if "exceptionDetails" in result:
            raise RuntimeError("Script failed: %s" % result["exceptionDetails"].get("exception", {}).get("description", result["exceptionDetails"]))
        return result["result"].get("value")

    def open(self, url):
        """Loads a page and waits until the document is there."""
        self.call("Page.navigate", url=url)
        self.wait_for("document.readyState == 'complete' && location.href != 'about:blank'")

    def wait_for(self, condition):
        """Waits until a JavaScript condition becomes true. Returns False if it does not within TIMEOUT.

        The waiting happens inside the page: a promise that tests the condition every
        50 ms and resolves as soon as it holds, or with false when time runs out.
        """
        return self.run("""
            new Promise(function (resolve) {
              var started = Date.now();
              (function test() {
                var ok = false;
                try { ok = !!(%s); } catch (error) {}
                if (ok) resolve(true);
                else if (Date.now() - started > %d) resolve(false);
                else setTimeout(test, 50);
              })();
            })""" % (condition, TIMEOUT * 1000))

    def close(self):
        """Shuts the browser down and removes its temporary profile."""
        try:
            self.connection.close()
        finally:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
            # The browser may still be releasing files in the folder for a moment.
            for _ in range(10):
                shutil.rmtree(self.profile, ignore_errors=True)
                if not os.path.exists(self.profile):
                    break
                time.sleep(0.3)


# Small pieces of JavaScript that read the state of the page, as a person would see it.
STATUS = "document.getElementById('status').textContent"
TABLE = "Array.from(document.getElementById('rows').rows).map(function (r) { return Array.from(r.cells).map(function (c) { return c.textContent; }); })"
HEADERS = "Array.from(document.getElementById('head').rows[0].cells).map(function (c) { return c.textContent; })"


def as_shown(rows):
    """Formats database rows the way the page displays them: everything as text,
    and the last two columns with exactly two decimals."""
    return [[str(row[0]), row[1], row[2], str(row[3]), row[4], "%.2f" % row[5], "%.2f" % row[6]] for row in rows]


def disabled(browser, button):
    return browser.run("document.getElementById('%s').disabled" % button)


def click(browser, element_id):
    browser.run("document.getElementById('%s').click()" % element_id)


def click_header(browser, column):
    browser.run("document.getElementById('head').rows[0].cells[%d].click()" % column)


def wait_status(browser, text):
    """Waits for the status line to read exactly the given text and reports whether it did."""
    return browser.wait_for("%s == %s" % (STATUS, json.dumps(text)))


def main():
    base = (sys.argv[1] if len(sys.argv) > 1 else "http://esp32.local").rstrip("/")
    executable = sys.argv[2] if len(sys.argv) > 2 else next((path for path in BROWSERS if os.path.exists(path)), None)
    if not executable:
        sys.exit("No browser found. Pass the path to msedge.exe or chrome.exe as the second argument.")

    # The reference data, read on the PC from the same file that is on the board.
    database = sqlite3.connect("file:" + os.path.join(PROJECT, "data", "babyname.db").replace("\\", "/") + "?mode=ro", uri=True)
    bob = [list(row) for row in database.execute(RANGE_SQL, ("Bob", "Bobby"))]
    mar = [list(row) for row in database.execute(RANGE_SQL.replace("between ? and ?", "like ? escape '\\'"), ("Mar%",))]

    # The board sends 200 rows per page (DB_PAGE_ROWS in include/Definitions.h).
    # The "Mar" search is used because it spans exactly two pages.
    page_size = 200
    if not (page_size < len(mar) <= 2 * page_size):
        sys.exit("The test expects names starting with 'Mar' to fill two pages; found %d rows" % len(mar))

    print("Browser: %s" % executable)
    browser = Browser(executable)
    try:
        # --- The page without a search ------------------------------------------
        print("\nOpening the page")
        browser.open(base + "/")
        check(browser.wait_for("/^\\d+:\\d\\d:\\d\\d$/.test(document.getElementById('uptime').textContent)"),
              "the uptime is shown as hours:minutes:seconds")
        check(browser.run("document.getElementById('results').hidden"), "no results area without a search")
        check(browser.run("document.getElementById('problem').hidden"), "no problem is reported")

        # --- A search given in the address --------------------------------------
        print("\nA range in the address")
        browser.open(base + "/?from=Bob&to=Bobby")
        check(wait_status(browser, "Rows 1 to %d of %d" % (len(bob), len(bob))), "the status shows the rows and the total")
        check(browser.run(TABLE) == as_shown(bob), "the table shows the %d rows of the database" % len(bob))
        check(browser.run("[document.getElementById('from').value, document.getElementById('to').value]") == ["Bob", "Bobby"],
              "the search is shown in the form fields")
        check(disabled(browser, "previous") and disabled(browser, "next"), "both paging buttons are off for a single page")

        print("\nThe address of the former results page")
        browser.open(base + "/query_db?from=Bob&to=Bobby")
        check(wait_status(browser, "Rows 1 to %d of %d" % (len(bob), len(bob))) and browser.run(TABLE) == as_shown(bob),
              "it shows the same results")

        # --- Paging --------------------------------------------------------------
        print("\nPaging through names starting with 'Mar'")
        browser.open(base + "/?prefix=Mar")
        first_status = "Rows 1 to %d of %d" % (page_size, len(mar))
        second_status = "Rows %d to %d of %d" % (page_size + 1, len(mar), len(mar))
        check(wait_status(browser, first_status), "first page: " + first_status)
        check(browser.run(TABLE) == as_shown(mar[:page_size]), "first page shows the first %d rows" % page_size)
        check(disabled(browser, "previous") and not disabled(browser, "next"), "only 'next' is available")

        click(browser, "next")
        check(wait_status(browser, second_status), "second page: " + second_status)
        check(browser.run(TABLE) == as_shown(mar[page_size:]), "second page shows the remaining %d rows" % (len(mar) - page_size))
        check(not disabled(browser, "previous") and disabled(browser, "next"), "only 'previous' is available")

        click(browser, "previous")
        check(wait_status(browser, first_status) and browser.run(TABLE) == as_shown(mar[:page_size]),
              "'previous' returns to the first page")

        # The first page was fetched twice by now. The second time it should have
        # come from the browser's cache, which shows as a transfer size of zero.
        sizes = browser.run("performance.getEntriesByType('resource').filter(function (e) { return e.name.indexOf('/api/names') >= 0 && e.name.indexOf('after_name') < 0; }).map(function (e) { return e.transferSize; })")
        check(len(sizes) == 2 and sizes[0] > 0 and sizes[1] == 0, "the repeated page came from the browser's cache (transfer sizes %s)" % sizes)

        # --- Sorting -------------------------------------------------------------
        # Sorting rearranges the rows on screen only. Both the page and Python sort
        # "stably" (rows that compare equal keep their order), so the expected
        # result can be computed here.
        print("\nSorting the rows shown")
        shown = mar[:page_size]

        click_header(browser, 3)
        check(browser.run(TABLE) == as_shown(sorted(shown, key=lambda row: row[3])), "a click on 'Total babies' sorts ascending")
        check(browser.run(HEADERS)[3] == "Total babies \u25b2", "the column heading shows an up arrow")

        click_header(browser, 3)
        check(browser.run(TABLE) == as_shown(sorted(shown, key=lambda row: row[3], reverse=True)), "a second click sorts descending")
        check(browser.run(HEADERS)[3] == "Total babies \u25bc", "the column heading shows a down arrow")

        click_header(browser, 3)
        check(browser.run(TABLE) == as_shown(shown), "a third click restores the original order")
        check(browser.run(HEADERS)[3] == "Total babies", "the arrow is gone")

        click_header(browser, 4)
        check(browser.run(TABLE) == as_shown(sorted(shown, key=lambda row: row[4])), "a text column sorts too ('Primary Sex')")

        click(browser, "next")
        check(wait_status(browser, second_status) and browser.run(TABLE) == as_shown(sorted(mar[page_size:], key=lambda row: row[4])),
              "the sort order is kept when moving to the next page")
        click_header(browser, 4)
        click_header(browser, 4)

        # --- Searching without reloading the page -------------------------------
        # A marker is stored in the page. A reload would wipe it out, so finding
        # it again afterwards proves the page was not reloaded.
        print("\nSearching from the form")
        browser.open(base + "/")
        browser.wait_for("document.getElementById('uptime').textContent != '...'")
        browser.run("window.testMarker = 'still here'")

        browser.run("document.getElementById('prefix').value = 'bob'; document.getElementById('prefixForm').requestSubmit()")
        check(wait_status(browser, "Rows 1 to %d of %d" % (len(bob), len(bob))) and browser.run(TABLE) == as_shown(bob),
              "'starts with' in lower case finds the names")
        check(browser.run("location.search") == "?prefix=bob", "the address shows the search")
        check(browser.run("window.testMarker") == "still here", "the page was not reloaded")

        browser.run("document.getElementById('from').value = 'Bob'; document.getElementById('to').value = 'Bobby'; document.getElementById('rangeForm').requestSubmit()")
        check(browser.wait_for("location.search == '?from=Bob&to=Bobby'") and wait_status(browser, "Rows 1 to %d of %d" % (len(bob), len(bob))),
              "the range form searches as well")

        browser.run("document.getElementById('prefix').value = 'zz'; document.getElementById('prefixForm').requestSubmit()")
        check(wait_status(browser, "No records found"), "a search without matches says so")
        check(browser.run("document.getElementById('table').hidden"), "and shows no table")

        # The browser's back button, stepping back through the three searches.
        browser.run("history.back()")
        check(browser.wait_for("location.search == '?from=Bob&to=Bobby'") and wait_status(browser, "Rows 1 to %d of %d" % (len(bob), len(bob))),
              "'back' returns to the previous search")
        browser.run("history.back()")
        browser.wait_for("location.search == '?prefix=bob'")
        browser.run("history.back()")
        check(browser.wait_for("location.search == '' && document.getElementById('results').hidden"),
              "'back' to the start hides the results")
        check(browser.run("window.testMarker") == "still here", "all of it without reloading the page")
    finally:
        browser.close()

    print("\n%s" % ("ALL CHECKS PASSED" if failures == 0 else "%d CHECKS FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
