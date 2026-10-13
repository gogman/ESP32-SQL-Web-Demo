"""Checks the web application on the board against the database file.

Usage (from the project folder):
    python tools/test_api.py [http://address-of-the-board]

The default address is http://esp32.local.

For a set of name ranges, every page is fetched from /api/names and the rows are
compared with what the same query gives on data/babyname.db. The script also checks
the row counts from /api/count, case-insensitive and "starts with" searches, the
error responses and the caching headers, and prints how long the requests took.

The page itself, as it behaves in a browser, is checked by tools/test_page.py.

The exit code is 0 when every check passed and 1 otherwise.

Note on the timings: with the default address, Windows looks up the name
"esp32.local" again for every request, which adds about a second each time. Pass
the board's IP address instead to see how fast the board itself answers.
"""

import json
import os
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# The project folder: the parent of the folder this script is in. Used to find
# the database file regardless of where the script is started from.
PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The reference query, run on the PC against the same database file that was
# uploaded to the board. It selects the same columns in the same order as the
# firmware (PAGE_SQL in src/db_mgr.cpp) but returns the whole range at once,
# without paging. If the board's pages, joined together, equal this result,
# then the paging lost nothing, repeated nothing and kept the order.
RANGE_SQL = ("Select year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state "
             "from gendered_names where name between ? and ? order by name, year, state")

# The name ranges to compare, as (from, to). They were chosen to cover the
# different situations: no rows at all (Zzz..Zzzz), a handful of rows, exactly
# one page, several pages, a very large range (A..B, about 3600 rows), and a
# range typed in lower case to check that case is ignored.
RANGES = [("Zzz", "Zzzz"), ("Bob", "Bobby"), ("Aaron", "Abel"), ("Ja", "Jb"),
          ("M", "Mb"), ("A", "Ab"), ("S", "T"), ("A", "B"), ("bob", "bobby")]

# Beginnings of names for the "starts with" search: lower case, mixed case with
# more than one page of results, one that matches nothing, and a single letter.
PREFIXES = ["bob", "Mar", "zz", "Q"]

# The number of checks that did not pass; decides the final message and exit code.
failures = 0


def check(ok, what):
    """Prints one result line, "ok" or "FAIL" followed by a description, and counts failures."""
    global failures
    if not ok:
        failures += 1
    print("%-4s %s" % ("ok" if ok else "FAIL", what))


def request(base, path, headers=None):
    """Returns (status, headers, body, milliseconds)."""
    call = urllib.request.Request(base + path, headers=headers or {})
    start = time.perf_counter()
    try:
        with urllib.request.urlopen(call, timeout=60) as response:
            status, reply_headers, body = response.status, response.headers, response.read()
    except urllib.error.HTTPError as error:
        # urllib raises an exception for every status of 400 and above. For this
        # script those are ordinary answers to be inspected (several checks
        # expect a 400, 404 or 405), so the exception is unpacked into the same
        # three values a successful response gives.
        status, reply_headers, body = error.code, error.headers, error.read()
    return status, reply_headers, body, (time.perf_counter() - start) * 1000


def fetch_all(base, query):
    """Pages through a query. Returns (rows, pages, milliseconds of first page, of all pages, bytes)."""
    rows, pages, first, total, size = [], 0, None, 0.0, 0

    # The parameters of the first request are the query alone. For every later
    # page the cursor is added; "query" itself is never modified.
    params = dict(query)
    while True:
        status, _, body, ms = request(base, "/api/names?" + urllib.parse.urlencode(params))
        pages += 1
        total += ms
        size += len(body)

        # Remember the time of the first page separately: it is what a person
        # using the page actually waits for.
        first = ms if first is None else first

        data = json.loads(body)
        if status != 200 or "error" in data:
            raise RuntimeError("HTTP %d: %s" % (status, data.get("error")))
        rows += data["rows"]

        # "more" is the board's way of saying whether another page exists.
        if not data["more"]:
            return rows, pages, first, total, size

        # Ask for the next page exactly the way the web page does: continue
        # after the last row received. A row is [year, state, name, ...], so the
        # name is at index 2, the year at index 0 and the state at index 1.
        last = data["rows"][-1]
        params = dict(query, after_name=last[2], after_year=last[0], after_state=last[1])


def fetch_count(base, query):
    """Returns the number of rows /api/count reports for a query."""
    status, _, body, _ = request(base, "/api/count?" + urllib.parse.urlencode(query))
    data = json.loads(body)
    if status != 200 or "error" in data:
        raise RuntimeError("HTTP %d: %s" % (status, data.get("error")))
    return data["count"]


def main():
    # The board's address without a trailing slash, since every path used below
    # starts with one.
    base = (sys.argv[1] if len(sys.argv) > 1 else "http://esp32.local").rstrip("/")

    # The database file on the PC, opened read-only. It must be the same file
    # that was uploaded to the board, or the comparisons below mean nothing.
    database = sqlite3.connect("file:" + os.path.join(PROJECT, "data", "babyname.db").replace("\\", "/") + "?mode=ro", uri=True)

    # --- The device status -------------------------------------------------
    # Printed in full because it documents what was tested: the firmware build,
    # the board's address, whether the database opened and how much memory is free.
    status, _, body, _ = request(base, "/api/status")
    check(status == 200, "status answers")
    state = json.loads(body)
    print("     " + ", ".join("%s=%s" % item for item in state.items()))
    check(state.get("fs_mounted") is True, "filesystem is mounted")
    check(state.get("db_open") is True, "database is open")

    # --- Ranges ------------------------------------------------------------
    # The board returns each row as a JSON array, which Python reads as a list;
    # sqlite3 returns tuples. Converting the reference rows to lists makes the
    # two directly comparable with ==, which also compares the order.
    print("\nRanges")
    for low, high in RANGES:
        want = [list(row) for row in database.execute(RANGE_SQL, (low, high))]
        try:
            got, pages, first, total, size = fetch_all(base, {"from": low, "to": high})
            check(got == want, "%-12s %5d rows, %2d pages, first page %4.0f ms, all pages %5.0f ms, %7d bytes"
                  % (low + ".." + high, len(got), pages, first, total, size))

            # The count the page shows as "of N" must agree with the rows themselves.
            count = fetch_count(base, {"from": low, "to": high})
            check(count == len(want), "%-12s counted as %d" % (low + ".." + high, count))
        except Exception as error:
            # A failure in one range is reported and the remaining ranges still run.
            check(False, "%s..%s: %s" % (low, high, error))

    # --- Names starting with -----------------------------------------------
    # The reference here is computed in a different way from the board on
    # purpose. The board turns a prefix into a range of names; the reference
    # uses SQL's LIKE with a "%" wildcard. Two independent methods agreeing is
    # stronger evidence than the same method run twice.
    print("\nNames starting with")
    for prefix in PREFIXES:
        want = [list(row) for row in database.execute(RANGE_SQL.replace("between ? and ?", "like ? escape '\\'"), (prefix + "%",))]
        try:
            got, pages, first, total, size = fetch_all(base, {"prefix": prefix})
            check(got == want, "%-12s %5d rows, %2d pages, first page %4.0f ms" % (prefix + "*", len(got), pages, first))
            count = fetch_count(base, {"prefix": prefix})
            check(count == len(want), "%-12s counted as %d" % (prefix + "*", count))
        except Exception as error:
            check(False, "%s*: %s" % (prefix, error))

    # --- Responses ---------------------------------------------------------
    print("\nResponses")

    # Neither a prefix nor a from/to pair: the board must refuse with status
    # 400 and still answer in JSON, so the page can display the reason.
    status, headers, body, _ = request(base, "/api/names")
    check(status == 400 and "error" in json.loads(body), "a request without a range is answered 400 with an error")

    # The same rule for the count.
    status, _, body, _ = request(base, "/api/count")
    check(status == 400 and "error" in json.loads(body), "a count without a range is answered 400 with an error")

    # Caching of the rows. A request carrying the database version ("v") may be
    # kept by the browser; one without it may not. Only the presence of "v"
    # matters to the board, so any value serves for the test.
    status, headers, _, _ = request(base, "/api/names?from=Bob&to=Bobby&v=test")
    check("max-age" in (headers.get("Cache-Control") or ""), "a versioned page may be cached (%s)" % headers.get("Cache-Control"))
    status, headers, _, _ = request(base, "/api/names?from=Bob&to=Bobby")
    check("no-cache" in (headers.get("Cache-Control") or ""), "an unversioned page is not cached (%s)" % headers.get("Cache-Control"))

    # The application page. First a plain request: it must arrive whole (ending
    # with the closing html tag) and carry an ETag, the label for its version.
    # Then the request a browser makes on a revisit, sending that label back:
    # the board must answer 304 "not modified" with an empty body.
    status, headers, body, ms = request(base, "/")
    etag = headers.get("ETag")
    check(status == 200 and body.rstrip().endswith(b"</html>") and etag, "the page is served complete, %d bytes in %.0f ms" % (len(body), ms))
    status, _, body, ms = request(base, "/", {"If-None-Match": etag or ""})
    check(status == 304 and not body, "an unchanged page is answered 304 in %.0f ms" % ms)

    # The address of the results page in earlier versions; kept working so old
    # links and bookmarks do not break.
    status, _, _, _ = request(base, "/query_db?from=Bob&to=Bobby")
    check(status == 200, "the old results address still works")

    # The files folder. These two checks expect the file data/files/read.me,
    # beginning with "Hello", to be in the filesystem image on the board.
    status, _, body, _ = request(base, "/files/")
    check(status == 200 and b"read.me" in body, "the files folder is listed")
    status, _, body, _ = request(base, "/files/read.me")
    check(status == 200 and body.startswith(b"Hello"), "a file downloads")

    # An attempt to climb out of the files folder with ".." and fetch the
    # database itself. It must be refused.
    status, _, _, _ = request(base, "/files/../babyname.db")
    check(status == 404, "nothing outside the files folder can be downloaded")
    status, _, _, _ = request(base, "/nothing-here")
    check(status == 404, "an unknown address is answered 404")

    # A POST. Giving urllib a "data" argument is what turns the request into a
    # POST. The board accepts only GET and must answer 405 "method not allowed".
    # This request is made directly rather than through request() because that
    # helper has no way to send a body.
    post = urllib.request.Request(base + "/api/status", data=b"x=1")
    try:
        status = urllib.request.urlopen(post, timeout=30).status
    except urllib.error.HTTPError as error:
        status = error.code
    check(status == 405, "a POST is answered 405")

    # The verdict, and an exit code other programs can test.
    print("\n%s" % ("ALL CHECKS PASSED" if failures == 0 else "%d CHECKS FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
