"""
Compresses the application page before every build.

PlatformIO runs this script itself, before compiling (extra_scripts in
platformio.ini). It reads web/index.html and writes web/index.html.gz, the
copy that is built into the firmware and sent to the browser. The .gz file
is generated, so it is not kept in version control; edit index.html only.

The file is only rewritten when its content changes, so an unchanged page
does not make the firmware link again.
"""

import gzip
import pathlib

Import("env")  # noqa: F821 - provided by PlatformIO's build system

SOURCE = pathlib.Path(env.subst("$PROJECT_DIR")) / "web" / "index.html"  # noqa: F821
TARGET = SOURCE.with_name("index.html.gz")

# mtime=0 leaves the time of compression out of the file, so the same page
# always gives the same bytes. The page's ETag is worked out from these bytes.
packed = gzip.compress(SOURCE.read_bytes(), compresslevel=9, mtime=0)

if not TARGET.exists() or TARGET.read_bytes() != packed:
    TARGET.write_bytes(packed)

print("Page: %s, %d bytes, sent as %d bytes" % (SOURCE.name, SOURCE.stat().st_size, len(packed)))
