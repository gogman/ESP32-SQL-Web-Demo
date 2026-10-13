"""
Compresses the Bootstrap stylesheet for the board.

Reads web/bootstrap.min.css and writes data/bootstrap.min.css.gz, the file
the firmware serves at /bootstrap.min.css. Run it after replacing the
stylesheet with another version, then upload the filesystem:

    python tools/pack_css.py
    pio run -e Release -t uploadfs

Only the standard library is used.
"""

import gzip
import pathlib

PROJECT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = PROJECT / "web" / "bootstrap.min.css"
TARGET = PROJECT / "data" / "bootstrap.min.css.gz"


def main():
    css = SOURCE.read_bytes()

    # mtime=0 leaves the time of compression out of the file, so the same
    # stylesheet always gives the same bytes and version control sees no
    # change when nothing changed.
    TARGET.write_bytes(gzip.compress(css, compresslevel=9, mtime=0))

    print("%s: %d bytes" % (SOURCE.relative_to(PROJECT), len(css)))
    print("%s: %d bytes" % (TARGET.relative_to(PROJECT), TARGET.stat().st_size))


if __name__ == "__main__":
    main()
