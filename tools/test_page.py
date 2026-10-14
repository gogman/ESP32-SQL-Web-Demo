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
        script = """
            new Promise(function (resolve) {
              var started = Date.now();
              (function test() {
                var ok = false;
                try { ok = !!(%s); } catch (error) {}
                if (ok) resolve(true);
                else if (Date.now() - started > %d) resolve(false);
                else setTimeout(test, 50);
              })();
            })""" % (condition, TIMEOUT * 1000)

        # If the browser moves to another document while the promise is
        # waiting, for example when a slow navigation finally goes through,
        # the promise is lost and the browser reports an error. The wait is
        # then simply started again in the new document.
        deadline = time.monotonic() + TIMEOUT
        while True:
            try:
                return self.run(script)
            except RuntimeError as error:
                if "navigated or closed" not in str(error) or time.monotonic() > deadline:
                    raise

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


def pager_button(button, pager="pagerTop"):
    """JavaScript that finds a button of one of the two pagers: 'first',
    'previous', 'next' or 'last' by name, or a page by its number."""
    if isinstance(button, int):
        return "document.querySelector('#%s button[data-page=\"%d\"]')" % (pager, button - 1)
    return "document.querySelector('#%s button[data-go=%s]')" % (pager, button)


def disabled(browser, button, pager="pagerTop"):
    return browser.run(pager_button(button, pager) + ".disabled")


def click(browser, button, pager="pagerTop"):
    browser.run(pager_button(button, pager) + ".click()")


def choose(browser, select, value):
    """Picks a value in a drop-down list, the way a person would."""
    browser.run("var s = document.getElementById('%s'); s.value = '%s'; s.dispatchEvent(new Event('change'))" % (select, value))


def open_at(browser, url, rows):
    """Opens the page and picks a number of rows per page. The page always
    opens at 25; choosing another number starts its search again."""
    browser.open(url)
    choose(browser, "perPageTop", str(rows))


def pager_labels(browser, pager="pagerTop"):
    """The labels of a pager's items, with the current page in brackets."""
    return browser.run("Array.from(document.getElementById('%s').children).map(function (item) {"
                       " return item.classList.contains('active') ? '[' + item.textContent + ']' : item.textContent; })" % pager)


def click_header(browser, column):
    browser.run("document.getElementById('head').rows[0].cells[%d].click()" % column)


def wait_table(browser, rows):
    """Waits for the table to show exactly the given database rows and reports whether it did."""
    expected = json.dumps(as_shown(rows), separators=(",", ":"), ensure_ascii=False)
    return browser.wait_for("JSON.stringify(%s) == %s" % (TABLE, json.dumps(expected)))


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
    s_to_t = [list(row) for row in database.execute(RANGE_SQL, ("S", "T"))]

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
        UPTIME = "document.getElementById('uptime').textContent"
        check(browser.wait_for("/^\\d+d \\d+:\\d\\d:\\d\\d$/.test(%s)" % UPTIME),
              "the uptime is shown as days, hours:minutes:seconds (%s)" % browser.run(UPTIME))

        # It counts on by itself, once a second, without asking the board again.
        def uptime_seconds():
            days, clock = browser.run(UPTIME).split("d ")
            hours, minutes, seconds = clock.split(":")
            return ((int(days) * 24 + int(hours)) * 60 + int(minutes)) * 60 + int(seconds)

        before = uptime_seconds()
        time.sleep(3.2)
        after = uptime_seconds()
        check(after - before in (3, 4), "it counts on by itself: 3.2 s later it shows %d s more" % (after - before))
        check([browser.run("formatUptime(%d)" % s) for s in (0, 59, 3725, 86399, 90061, 1000000)]
              == ["0d 0:00:00", "0d 0:00:59", "0d 1:02:05", "0d 23:59:59", "1d 1:01:01", "11d 13:46:40"],
              "days, hours, minutes and seconds are worked out correctly, e.g. 90061 s as 1d 1:01:01")

        # Bootstrap defines a set of colour variables on the document. If one
        # of them has a value, the browser fetched the stylesheet from the
        # board, unpacked it and applied it.
        check(browser.wait_for("getComputedStyle(document.documentElement).getPropertyValue('--bs-blue').trim() != ''"),
              "the Bootstrap stylesheet is applied")
        check(browser.run("document.getElementById('results').hidden"), "no results area without a search")
        check(browser.run("document.getElementById('problem').hidden"), "no problem is reported")

        # --- Light and dark theme ------------------------------------------------
        # The page sets Bootstrap's data-bs-theme on the document. "Auto" follows
        # the system setting, which the browser can be told to pretend; a choice
        # is remembered in the browser and survives a reload.
        print("\nLight and dark theme")
        THEME = "document.documentElement.getAttribute('data-bs-theme')"
        BACKGROUND = "getComputedStyle(document.body).backgroundColor"

        def system_theme(scheme):
            browser.call("Emulation.setEmulatedMedia", features=[{"name": "prefers-color-scheme", "value": scheme}])

        system_theme("light")
        check(browser.run("document.getElementById('themeAuto').checked") and browser.wait_for(THEME + " == 'light'"),
              "'Auto' is chosen at first and follows a light system setting")
        light_background = browser.run(BACKGROUND)
        system_theme("dark")
        check(browser.wait_for(THEME + " == 'dark'"), "and switches to dark when the system does")
        check(browser.run(BACKGROUND) != light_background, "the background changes with it (%s, was %s)" % (browser.run(BACKGROUND), light_background))

        browser.run("document.getElementById('themeLight').click()")
        check(browser.wait_for(THEME + " == 'light'"), "'Light' overrides the dark system setting")
        browser.open(base + "/")
        check(browser.wait_for(THEME + " == 'light'") and browser.run("document.getElementById('themeLight').checked"),
              "and is still chosen after a reload")

        system_theme("light")
        browser.run("document.getElementById('themeDark').click()")
        check(browser.wait_for(THEME + " == 'dark'"), "'Dark' overrides a light system setting")

        browser.run("document.getElementById('themeAuto').click()")
        check(browser.wait_for(THEME + " == 'light'"), "'Auto' follows the system again")

        # --- The loading overlay -------------------------------------------------
        # While the page waits for rows, a spinner with "Loading..." covers the
        # page, centred, over a darkened background. It fades in after 0.15 s, so
        # a fast answer does not flash it; the layout is there before that.
        print("\nThe loading overlay")
        LOADING = "document.getElementById('loading')"
        check(browser.run("getComputedStyle(%s).visibility" % LOADING) == "hidden", "it is hidden while nothing is loading")
        shown = browser.run("document.getElementById('prefix').value = 'Mar'; document.getElementById('prefixForm').requestSubmit(); "
                            "%s.classList.contains('show')" % LOADING)
        check(shown, "a search shows it at once")
        check(browser.wait_for("!%s.classList.contains('show')" % LOADING) and wait_status(browser, "Rows 1 to 25 of %d" % len(mar)),
              "and it goes away when the rows are there")

        browser.run("showLoading(true)")
        check(browser.wait_for("getComputedStyle(%s).opacity == '1'" % LOADING), "it fades in fully while loading takes longer")
        box = browser.run("(function () { var r = %s.firstElementChild.getBoundingClientRect();"
                          " return [r.left + r.width / 2 - document.documentElement.clientWidth / 2, r.top + r.height / 2 - innerHeight / 2,"
                          " %s.getBoundingClientRect().width == document.documentElement.clientWidth,"
                          " %s.getBoundingClientRect().height == innerHeight]; })()" % (LOADING, LOADING, LOADING))
        check(abs(box[0]) <= 1 and abs(box[1]) <= 1 and box[2] and box[3],
              "it covers the window, with the spinner centred both ways (off by %.1f, %.1f px)" % (box[0], box[1]))
        check(browser.run("getComputedStyle(%s).backgroundColor" % LOADING) == "rgba(0, 0, 0, 0.25)", "the background darkens the page slightly")
        check(browser.run("%s.querySelector('.spinner-border') != null && %s.textContent.trim() == 'Loading...'" % (LOADING, LOADING)),
              "the spinner has 'Loading...' below it")
        LOOK = ("(function () { var box = %s.firstElementChild; return [getComputedStyle(box).backgroundColor, getComputedStyle(box).color,"
                " getComputedStyle(box.querySelector('.spinner-border')).color]; })()" % LOADING)
        light_look = browser.run(LOOK)
        browser.run("document.getElementById('themeDark').click()")
        browser.wait_for(THEME + " == 'dark'")
        dark_look = browser.run(LOOK)
        check(light_look[0] != dark_look[0] and light_look[1] != dark_look[1],
              "its box and text follow the theme (light %s on %s, dark %s on %s)" % (light_look[1], light_look[0], dark_look[1], dark_look[0]))
        browser.run("document.getElementById('themeAuto').click()")
        browser.wait_for(THEME + " == 'light'")
        browser.run("showLoading(false)")
        check(browser.wait_for("getComputedStyle(%s).visibility == 'hidden'" % LOADING), "it can be hidden again")

        # --- Rows per page and the primary sex filter ---------------------------
        print("\nRows per page and the primary sex filter")
        mar_f = [row for row in mar if row[4] == "F"]
        mar_m = [row for row in mar if row[4] == "M"]

        def rows_of(start, count, of):
            return "Rows %d to %d of %d" % (start + 1, start + count, of)

        browser.open(base + "/?prefix=Mar")
        check(wait_status(browser, rows_of(0, 25, len(mar))) and wait_table(browser, mar[:25]), "25 rows per page at first: " + rows_of(0, 25, len(mar)))
        check(browser.run("[document.getElementById('perPageTop').value, document.getElementById('perPageBottom').value]") == ["25", "25"],
              "both 'Rows per page' lists show 25")

        # 10 is chosen for the filter checks below, so they also show that a
        # different choice takes effect.
        choose(browser, "perPageBottom", "10")
        check(wait_table(browser, mar[:10]) and wait_status(browser, rows_of(0, 10, len(mar))), "10 chosen below the table: " + rows_of(0, 10, len(mar)))
        check(browser.run("document.getElementById('perPageTop').value") == "10", "the list above the table follows")
        click(browser, "next")
        check(wait_table(browser, mar[10:20]), "'next' shows rows 11 to 20")

        choose(browser, "sex", "F")
        check(wait_table(browser, mar_f[:10]) and wait_status(browser, rows_of(0, 10, len(mar_f))),
              "'Female' keeps only female rows: " + rows_of(0, 10, len(mar_f)))
        last_f = (len(mar_f) - 1) // 10 * 10
        click(browser, "last")
        check(wait_table(browser, mar_f[last_f:]), "'last' shows the last %d female rows" % (len(mar_f) - last_f))
        choose(browser, "sex", "M")
        check(wait_table(browser, mar_m[:10]) and wait_status(browser, rows_of(0, 10, len(mar_m))),
              "'Male' keeps only male rows: " + rows_of(0, 10, len(mar_m)))
        choose(browser, "sex", "")
        check(wait_table(browser, mar[:10]), "'All' shows every row again")

        # The choice is not remembered: a reload opens at 25 again.
        choose(browser, "perPageTop", "200")
        check(wait_status(browser, rows_of(0, 200, len(mar))), "200 chosen: " + rows_of(0, 200, len(mar)))
        browser.open(base + "/?prefix=Mar")
        check(wait_status(browser, rows_of(0, 25, len(mar))) and browser.run("[document.getElementById('perPageTop').value, document.getElementById('perPageBottom').value]") == ["25", "25"],
              "a reload opens at 25 rows per page again")

        # The rest of this test was written for 200 rows per page, so every page
        # it opens from here on is switched to 200 first (open_at).

        # The crash notice must be shown exactly when the board says a crash
        # dump is waiting. The board is asked from inside the page.
        waiting = browser.run("fetch('/api/status').then(function (r) { return r.json(); }).then(function (s) { return s.crash_dump; })")
        check(browser.run("document.getElementById('crash').hidden") == (not waiting),
              "the crash notice is %s, matching the board (crash dump waiting: %s)" % ("shown" if waiting else "hidden", waiting))

        # --- A search given in the address --------------------------------------
        print("\nA range in the address")
        open_at(browser, base + "/?from=Bob&to=Bobby", page_size)
        check(wait_status(browser, "Rows 1 to %d of %d" % (len(bob), len(bob))), "the status shows the rows and the total")
        check(browser.run(TABLE) == as_shown(bob), "the table shows the %d rows of the database" % len(bob))
        check(browser.run("[document.getElementById('from').value, document.getElementById('to').value]") == ["Bob", "Bobby"],
              "the search is shown in the form fields")
        check(all(disabled(browser, button) for button in ("first", "previous", "next", "last")),
              "all four paging buttons are off for a single page")
        check(browser.run("document.getElementById('pagerTop').hidden && document.getElementById('pagerBottom').hidden"),
              "and both pagers are hidden")

        print("\nThe address of the former results page")
        open_at(browser, base + "/query_db?from=Bob&to=Bobby", page_size)
        check(wait_status(browser, "Rows 1 to %d of %d" % (len(bob), len(bob))) and browser.run(TABLE) == as_shown(bob),
              "it shows the same results")

        # --- Paging --------------------------------------------------------------
        print("\nPaging through names starting with 'Mar'")
        open_at(browser, base + "/?prefix=Mar", page_size)
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

        # The first page has been fetched at least twice by now; an earlier
        # section may have fetched it already. The repeat after 'previous' must
        # have come from the browser's cache, which shows as a transfer size of
        # zero.
        sizes = browser.run("performance.getEntriesByType('resource').filter(function (e) { return e.name.indexOf('/api/names') >= 0 && e.name.indexOf('after_name') < 0; }).map(function (e) { return e.transferSize; })")
        check(len(sizes) >= 2 and sizes[-1] == 0, "the repeated page came from the browser's cache (transfer sizes %s)" % sizes)

        # --- First and last ------------------------------------------------------
        # A range with many pages. "last" has to reach a page the browser has
        # never seen, and "previous" from there one it has no cursor for.
        print("\nJumping to the first and last page of S..T")
        pages = -(-len(s_to_t) // page_size)
        last_start = (pages - 1) * page_size

        def rows_status(start, count):
            return "Rows %d to %d of %d" % (start + 1, start + count, len(s_to_t))

        open_at(browser, base + "/?from=S&to=T", page_size)
        check(wait_status(browser, rows_status(0, page_size)), "first page: " + rows_status(0, page_size))
        check(disabled(browser, "first") and disabled(browser, "previous") and not disabled(browser, "next") and not disabled(browser, "last"),
              "on the first page only 'next' and 'last' are available")
        check(pager_labels(browser) == ["First", "Previous", "[1]", "2", "3", "Next", "Last"],
              "the pager above the table shows: First Previous [1] 2 3 Next Last")
        check(pager_labels(browser, "pagerBottom") == pager_labels(browser), "the pager below the table shows the same")

        # A page number, from the pager below the table. Page 3 has not been
        # seen, so it is fetched by position.
        click(browser, 3, "pagerBottom")
        check(wait_status(browser, rows_status(2 * page_size, page_size)) and browser.run(TABLE) == as_shown(s_to_t[2 * page_size:3 * page_size]),
              "a click on page 3 below the table shows: " + rows_status(2 * page_size, page_size))
        check(pager_labels(browser) == ["First", "Previous", "1", "2", "[3]", "4", "5", "Next", "Last"],
              "the pager shows: First Previous 1 2 [3] 4 5 Next Last")

        click(browser, "last")
        check(wait_status(browser, rows_status(last_start, len(s_to_t) - last_start)), "'last' shows: " + rows_status(last_start, len(s_to_t) - last_start))
        check(browser.run(TABLE) == as_shown(s_to_t[last_start:]), "with the last %d rows of the database" % (len(s_to_t) - last_start))
        check(not disabled(browser, "first") and not disabled(browser, "previous") and disabled(browser, "next") and disabled(browser, "last"),
              "on the last page only 'first' and 'previous' are available")

        click(browser, "previous")
        before_last = last_start - page_size
        check(wait_status(browser, rows_status(before_last, page_size)) and browser.run(TABLE) == as_shown(s_to_t[before_last:last_start]),
              "'previous' from the last page shows the page before it")
        check(not any(disabled(browser, button) for button in ("first", "previous", "next", "last")),
              "in the middle all four buttons are available")

        click(browser, "next")
        check(wait_status(browser, rows_status(last_start, len(s_to_t) - last_start)) and browser.run(TABLE) == as_shown(s_to_t[last_start:]),
              "'next' returns to the last page")

        click(browser, "first")
        check(wait_status(browser, rows_status(0, page_size)) and browser.run(TABLE) == as_shown(s_to_t[:page_size]),
              "'first' returns to the first page")

        # --- Sorting -------------------------------------------------------------
        # Sorting is done by the board and covers the whole search, not just the
        # rows on screen. The expected order is worked out here: Python sorts
        # "stably", so rows with equal values stay in name order, which is the
        # board's rule too; descending is that order backwards.
        print("\nSorting the whole search")
        open_at(browser, base + "/?prefix=Mar", page_size)
        wait_status(browser, first_status)
        by_total = sorted(mar, key=lambda row: row[3])
        by_sex = sorted(mar, key=lambda row: row[4])

        click_header(browser, 3)
        check(wait_table(browser, by_total[:page_size]), "a click on 'Total babies' shows the %d smallest of all %d rows" % (page_size, len(mar)))
        check(browser.run(HEADERS)[3] == "Total babies \u25b2", "the column heading shows an up arrow")
        check(browser.run(STATUS) == first_status, "the status line still reads: " + first_status)

        click(browser, "next")
        check(wait_table(browser, by_total[page_size:]) and wait_status(browser, second_status), "'next' shows the rest of the sorted rows")
        click(browser, "last")
        click(browser, "first")
        check(wait_table(browser, by_total[:page_size]), "'first' returns to the start of the sorted rows")

        click_header(browser, 3)
        check(wait_table(browser, by_total[::-1][:page_size]), "a second click shows the %d largest" % page_size)
        check(browser.run(HEADERS)[3] == "Total babies \u25bc", "the column heading shows a down arrow")

        click_header(browser, 3)
        check(wait_table(browser, mar[:page_size]), "a third click restores name order")
        check(browser.run(HEADERS)[3] == "Total babies", "the arrow is gone")

        click_header(browser, 4)
        check(wait_table(browser, by_sex[:page_size]), "a text column sorts too ('Primary Sex')")

        # The sort stays on for the next search.
        browser.run("document.getElementById('from').value = 'S'; document.getElementById('to').value = 'T'; document.getElementById('rangeForm').requestSubmit()")
        check(wait_table(browser, sorted(s_to_t, key=lambda row: row[4])[:page_size]), "the sort order is kept for the next search")
        click(browser, "last")
        check(wait_table(browser, sorted(s_to_t, key=lambda row: row[4])[last_start:]), "and 'last' shows the end of that sorted search")
        click_header(browser, 4)
        click_header(browser, 4)
        check(wait_table(browser, s_to_t[:page_size]), "two more clicks restore name order")

        # --- Searching without reloading the page -------------------------------
        # A marker is stored in the page. A reload would wipe it out, so finding
        # it again afterwards proves the page was not reloaded.
        print("\nSearching from the form")
        open_at(browser, base + "/", page_size)
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
