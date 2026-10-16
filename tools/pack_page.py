"""
Compresses the pages before every build.

PlatformIO runs this script itself, before compiling (extra_scripts in
platformio.ini). For each page in PAGES it reads web/<page>.html and writes
web/<page>.html.gz, the copy that is built into the firmware and sent to the
browser. The .gz files are generated, so they are not kept in version
control; edit the .html files only.

A file is only rewritten when its content changes, so an unchanged page
does not make the firmware link again.
"""

import gzip
import pathlib

Import("env")  # noqa: F821 - provided by PlatformIO's build system

# The application page and the setup page (/admin/setup/).
PAGES = ["index.html", "admin.html"]

WEB = pathlib.Path(env.subst("$PROJECT_DIR")) / "web"  # noqa: F821

for page in PAGES:
    source = WEB / page
    target = source.with_name(page + ".gz")

    # mtime=0 leaves the time of compression out of the file, so the same page
    # always gives the same bytes. The page's ETag is worked out from these bytes.
    packed = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)

    if not target.exists() or target.read_bytes() != packed:
        target.write_bytes(packed)

    print("Page: %s, %d bytes, sent as %d bytes" % (source.name, source.stat().st_size, len(packed)))
