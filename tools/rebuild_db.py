"""Builds data/babyname.db, the database the firmware reads, from the original data.

Usage (from the project folder):
    python tools/rebuild_db.py source.db [target.db]

source.db is a database with the table gendered_names in its original layout. The
target defaults to data/babyname.db. The original the current data was built from
is no longer kept in the project; it is in the repository's history and can be
taken out with:
    git show c60ca07:db_backup/babyname.original.db > babyname.original.db

The target holds the same rows as the source table gendered_names, laid out for the
ESP32: the table is stored in (name, year, state) order so a range of names is one
sequential read, names compare without regard to case, and pages are 4096 bytes to
match the flash block size. Every row is checked against the source before the
target replaces the previous file.

After running this, upload the new file to the board with:
    pio run -e Release -t uploadfs
"""

import os
import sqlite3
import sys

# The columns copied from the source, in the order of the new table below.
# The same text is used for reading the source, for reading the copy back and,
# through its position in CREATE_TABLE, for inserting, so the three always agree.
COLUMNS = "name, year, state, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state"

# The layout of the table on the board. Three things here are deliberate and
# the firmware's query (PAGE_SQL in src/db_mgr.cpp) depends on all of them:
#
# - WITHOUT ROWID together with PRIMARY KEY (name, year, state) makes SQLite
#   store the rows themselves sorted by that key. An ordinary table stores rows
#   in insertion order and needs a separate index to find names, which costs one
#   extra lookup per row; on the board's flash that lookup was the slowest part
#   of every query by far.
#
# - COLLATE NOCASE on name makes comparisons and the sort order ignore upper and
#   lower case (for the letters A to Z), so a search for "bob" finds "Bob".
#
# - name comes first in the key because names are what the application searches
#   by. year and state are included to make the key unique.
CREATE_TABLE = """
CREATE TABLE gendered_names (
    name  TEXT NOT NULL COLLATE NOCASE,
    year  NUMERIC NOT NULL,
    state TEXT NOT NULL,
    total_babies NUMERIC NOT NULL,
    primary_sex   TEXT NOT NULL,
    primary_sex_ratio NUMERIC NOT NULL,
    per_100k_in_state NUMERIC NOT NULL,
    PRIMARY KEY (name, year, state)
) WITHOUT ROWID
"""


def main():
    # The project folder is the parent of the folder this script is in, so the
    # default target works no matter which folder the script is started from.
    project = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    source = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else os.path.join(project, "data", "babyname.db")

    # The new database is built under a temporary name next to the target. The
    # existing target is only replaced at the very end, once the new file has
    # passed its checks, so a failed run never leaves a broken database behind.
    building = target + ".building"

    # Open the source read-only ("mode=ro") so this script cannot alter it under
    # any circumstances. The "file:" form of the name is what allows passing
    # that option, and it wants forward slashes even on Windows.
    original = sqlite3.connect("file:" + source.replace("\\", "/") + "?mode=ro", uri=True)
    rows = original.execute("SELECT %s FROM gendered_names" % COLUMNS).fetchall()

    # A leftover from an earlier run that was interrupted.
    if os.path.exists(building):
        os.remove(building)

    rebuilt = sqlite3.connect(building)
    try:
        # The page size can only be chosen while the database is still empty, so
        # this has to be the first statement. 4096 bytes equals the block size of
        # the board's filesystem, so reading one page is reading one block. The
        # source used 512-byte pages, which meant many more, smaller reads.
        rebuilt.execute("PRAGMA page_size = 4096")
        rebuilt.execute(CREATE_TABLE)

        # Copy every row. If two rows have the same name (ignoring case), year
        # and state, the primary key rejects the second one and the script stops
        # here with an error, leaving the target untouched.
        rebuilt.executemany("INSERT INTO gendered_names VALUES (?, ?, ?, ?, ?, ?, ?)", rows)
        rebuilt.commit()

        # VACUUM rewrites the file compactly, with every page full and in order,
        # which gives the smallest file and the fewest reads on the board.
        rebuilt.execute("VACUUM")

        # Verify the result before trusting it: SQLite's own structural check,
        # and a read-back of everything that was written.
        check = rebuilt.execute("PRAGMA integrity_check").fetchone()[0]
        copied = rebuilt.execute("SELECT %s FROM gendered_names" % COLUMNS).fetchall()
    finally:
        # Close the file in every case, so it can be removed or renamed below.
        rebuilt.close()

    # The two lists are sorted before comparing because the source returns rows
    # in insertion order and the copy returns them in key order; only the
    # content has to match, not the order.
    if check != "ok" or sorted(copied) != sorted(rows):
        os.remove(building)
        sys.exit("Rebuilt database does not match the source (integrity: %s); target left unchanged" % check)

    # Put the new file in place. os.replace swaps it in as a single step.
    os.replace(building, target)
    print("%s: %d rows, %d bytes (source %d bytes)" % (target, len(rows), os.path.getsize(target), os.path.getsize(source)))


if __name__ == "__main__":
    main()
