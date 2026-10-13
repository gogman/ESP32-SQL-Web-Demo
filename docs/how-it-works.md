# SQLite_Web: how it works

SQLite_Web is firmware for an ESP32 board that serves a small web application. The application searches a SQLite database of baby names stored on the board's own flash memory and shows the results in a browser, 200 rows at a time.

This document describes how the program is put together, how the web page works, how to build, test and diagnose it, what is known to be wrong or fragile, and what could be improved next. It reflects the state of the project on 13 October 2026.

## At a glance

| | |
|---|---|
| Board | Freenove ESP32-WROVER, 4 MB flash, 4 MB PSRAM |
| Framework | Arduino core, built with PlatformIO (platform 7.1.3) |
| Database | SQLite through the Sqlite3Esp32 library 2.5.0 (SQLite 3.43.2) |
| Data | One table, `gendered_names`, 25,192 rows, 835,584 bytes |
| Filesystem | LittleFS, 2048 KB partition |
| Address | `http://esp32.local`, or the board's IP address |
| Firmware size | Release about 67% and Debug about 72% of the 1920 KB firmware partition |
| Version control | Git repository in the project folder |

## Project layout

| Path | What it holds |
|---|---|
| `src/main.cpp` | Entry point: `setup()` and `loop()` |
| `src/network.cpp`, `include/network.h` | WiFi, the fallback access point, the `esp32.local` name |
| `src/db_mgr.cpp`, `include/db_mgr.h` | The database: opening it, producing pages of rows as JSON, counting rows |
| `src/web_handlers.cpp`, `include/web_handlers.h` | Every address the web server answers |
| `include/chunk_server.h` | A web server class that sends streamed responses efficiently |
| `include/Definitions.h` | All settings |
| `include/Secrets.h` | WiFi name and passwords; not kept in version control. `Secrets.example.h` is the template |
| `web/index.html` | The application page, built into the firmware |
| `data/` | The filesystem image: `babyname.db` and the `files` folder |
| `db_backup/babyname.original.db` | The original database that `data/babyname.db` is built from |
| `tools/rebuild_db.py` | Builds `data/babyname.db` from the original |
| `tools/test_api.py` | Checks the addresses of a running board against the database file |
| `tools/test_page.py` | Checks the page of a running board in a real browser |
| `defpart.csv` | The flash partition table |
| `platformio.ini` | Build, upload and monitor settings |

The C++ files, the Python tools and the two configuration files carry detailed comments. The web page carries none; its explanation is in [The web page](#the-web-page) below.

## Flash layout

`defpart.csv` divides the 4 MB chip as follows.

| Partition | Start | Size | Purpose |
|---|---|---|---|
| `nvs` | 0x9000 | 20 KB | System settings such as radio calibration |
| (boot data) | 0xE000 | 8 KB | Written by the upload tool with every firmware upload; not a partition |
| `factory` | 0x10000 | 1920 KB | The firmware, a single copy |
| `spiffs` | 0x1F0000 | 2048 KB | The LittleFS filesystem (the subtype name is historical) |
| `coredump` | 0x3F0000 | 64 KB | Crash dumps saved by the framework |

Changing this table moves the partitions. Afterwards both the firmware and the filesystem image must be uploaded again, or the board finds no filesystem where it expects one.

## Startup

`setup()` in `src/main.cpp` brings the board up in this order:

1. **LED and serial port**, so later steps can report progress at 115200 baud.
2. **WiFi.** The board tries the network from `Secrets.h` for up to 20 seconds. If that fails it starts its own access point named `esp32-sqlite`, where the application is reachable at `192.168.4.1`.
3. **Filesystem.** LittleFS is mounted. It is never formatted by the firmware. If it cannot be mounted, the board says so on the serial port, on the page and in `/api/status`, and carries on without it.
4. **Database.** `babyname.db` is opened read-only. With PSRAM present, SQLite gets a 1024 KiB page cache, which is larger than the whole database. If opening fails, the server still starts and every query answers with the error.
5. **Name on the network.** The board announces itself as `esp32.local`.
6. **Web server.** The handlers are registered and the server starts listening on port 80.

`loop()` then repeats three things: serve one waiting request, do network housekeeping, and sleep for a millisecond so the processor is not kept busy doing nothing.

Network housekeeping only matters in access-point mode: every five minutes the board restarts to try the WiFi network again, unless somebody is connected to the access point.

### When the filesystem is missing

A failed mount almost always means that no filesystem image has been uploaded to where the partition table says it is: a new board, or a changed partition table. The fix is `pio run -e Release -t uploadfs`.

This was tested by erasing the partition. The board started normally, `/api/status` reported `"fs_mounted":false` and `"db_error":"unable to open database file"`, and the page showed "The filesystem could not be mounted. Upload the filesystem image to the board." A second restart gave the same result, confirming that nothing had been formatted. After uploading the image, everything worked again.

## Addresses

All addresses accept `GET` only.

| Address | Answer |
|---|---|
| `/` | The application page |
| `/query_db` | The same page, kept so older links still work |
| `/api/names` | One page of names as JSON |
| `/api/count` | The number of names in a range as JSON |
| `/api/status` | The state of the device as JSON |
| `/files`, `/files/` | An HTML listing of the downloadable files |
| `/files/<name>` | One file from the `files` folder |
| anything else | `404`, or `405` for a method other than `GET` |

### `/api/names`

| Parameter | Meaning |
|---|---|
| `prefix` | Names starting with this text. Use this, or `from` and `to` |
| `from`, `to` | The first and last name of a range, both included |
| `after_name`, `after_year`, `after_state` | Optional. The last row of the previous page; the answer continues after it |
| `v` | Optional. The database version from `/api/status`. Its presence makes the answer cacheable |

The answer has this form:

```json
{"rows":[[2015,"US","Bob",14,"M",100,0.4],[2015,"US","Bobbi",67,"F",100,1.8]],"more":false}
```

Each row is `[year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state]`. `more` is `true` when rows remain beyond this page. Names are matched without regard to case.

A request without a range is answered `400`. A database failure before any output is answered `500`. Both carry `{"error":"..."}`. If a failure happens after output has started, the document is still completed as valid JSON with an added `error` member.

### `/api/count`

Takes the same `prefix` or `from` and `to` parameters, and `v`, and answers `{"count":1234}`. Errors are reported as for `/api/names`.

It is a separate address, and not part of every page of rows, because counting reads the whole range. The page asks for it once per search.

### `/api/status`

| Member | Meaning |
|---|---|
| `uptime` | Seconds since the board started |
| `build` | When `src/web_handlers.cpp` was compiled; a hint at which firmware is running |
| `network`, `ip` | `station` or `access point`, and the board's address |
| `fs_mounted` | Whether the filesystem could be mounted |
| `db_open`, `db_error` | Whether the database is open, and the reason if it is not |
| `db_version` | The database file's size and modification time; changes when the database is replaced |
| `db_cache_kib` | The size of SQLite's page cache |
| `heap_free`, `psram_size`, `psram_free`, `flash_size` | Memory figures in bytes |

### Files

The listing shows the files directly inside the `files` folder with their sizes. Subfolders are not listed. Any address containing `..` is refused, so nothing outside the folder, including the database, can be downloaded.

## The database

### Layout

`tools/rebuild_db.py` builds the database the board reads. Three properties of its layout matter, and the firmware's queries depend on all of them:

- **Rows are stored sorted by name.** The table is `WITHOUT ROWID` with the primary key `(name, year, state)`, so the rows themselves lie in that order. A range of names is one sequential read. The original layout needed a separate index lookup for every row, which cost about 5.8 ms per row on the board.
- **Names ignore case.** The `name` column uses the `NOCASE` collation, so `bob` finds `Bob`. This applies to the letters A to Z only.
- **Pages are 4096 bytes**, matching the block size of the filesystem.

### The queries

The firmware runs two queries, both compiled once when the database is opened.

Rows:

```sql
Select year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state
from gendered_names
where name between ?1 and ?2 and (name, year, state) > (?3, ?4, ?5)
order by name, year, state limit ?6
```

`?1` and `?2` bound the names. `?3` to `?5` are the cursor: the key of the last row already shown. `?6` is the row limit.

Count:

```sql
Select count(*) from gendered_names where name between ?1 and ?2
```

It uses the same `between`, so it counts exactly the rows that paging through the range returns.

### Paging with cursors

The board does not number its pages. Each request says "continue after this row". This keeps every page equally cheap: the query jumps straight to the cursor and does not skip the rows of earlier pages.

Two details make this work:

- The lower bound of the name range is replaced by the cursor's name when a cursor is given, so reading starts where the previous page ended.
- The query asks for one row more than the page size. If that extra row exists, more data follows and `more` is set to `true`. The extra row is not sent; it becomes the first row of the next page.

"Names starting with X" is run as the range from `X` to `X` followed by the highest possible character.

### Output

Rows are written as JSON directly into a fixed 1400-byte buffer inside the database object. When the buffer is full it is handed to the web handler, which sends it, and filling starts again. A query therefore allocates no memory of its own, and a result of any size can be streamed. Numbers are formatted by small hand-written routines, not `printf`. The two decimal columns are rounded to two places.

## Sending responses

Results are sent with HTTP chunked encoding, because their length is not known in advance. The stock web server writes each chunk as three separate network packets (length, data, line break). `ChunkServer` in `include/chunk_server.h` assembles the three parts in one buffer and writes them once, so a chunk is one packet. A full 1400-byte chunk with its framing still fits in a single TCP packet.

The HTTP status and headers of `/api/names` are sent together with the first chunk, not before the query starts. That is what allows a query that fails immediately to be answered with a proper `500`.

## Caching

- **The page** is sent with an `ETag` and `Cache-Control: no-cache`. The browser keeps its copy and asks each time whether it has changed; if not, the board answers `304 Not Modified` with no body. The `ETag` is computed at startup from the page itself (its length and an FNV-1a checksum of its content), so it changes exactly when the page changes.
- **Rows and counts** requested with the `v` parameter are sent with `Cache-Control: private, max-age=86400`, so the browser may reuse them for a day. The value of `v` is the database version. When a new database is uploaded the version changes, the request addresses change with it, and old cached answers are not reused.
- **Status and errors** are never cached.

## The web page

`web/index.html` is the whole user interface. It is not stored on the board's filesystem: the build embeds it into the firmware (`board_build.embed_txtfiles` in `platformio.ini`) and `src/web_handlers.cpp` sends it for `/` and `/query_db`. After changing the page, the firmware has to be rebuilt and uploaded; uploading the filesystem does not update it.

The file itself contains no comments, to keep it small on the wire. This section is its documentation.

### How a search works

The page is both the search form and the results. A search never reloads it:

- Submitting either form is intercepted by the script. The script writes the search into the address bar (for example `/?prefix=bob` or `/?from=Bob&to=Bobby`) with `history.pushState`, and loads the results.
- Opening such an address directly, or reloading the page, runs the same search from the address.
- The browser's back and forward buttons step through earlier searches without reloading.

Because the address always describes the search, results can be bookmarked and shared.

### Structure

- **Uptime line.** A `span` with the id `uptime` shows `...` until the script fills it in from `/api/status`.
- **Problem line.** A paragraph with the id `problem`, hidden unless the board reports that the filesystem is not mounted or the database is not open.
- **Two forms**, `prefixForm` and `rangeForm`. They still declare method `GET` and action `/`, which is what they would do without the script, but the script handles their submission. The `id` attributes of the fields let the script read them and write the current search back.
- **Files link.** Leads to the file listing, a separate page produced by the board.
- **Results area.** A `div` with the id `results`, hidden while there is no search. The table inside is hidden separately, so an empty result shows just the status line. The table heading (`head`) is clickable for sorting.

### Script variables

| Variable | Purpose |
|---|---|
| `COLUMNS` | The column headings, used to redraw them with or without a sort arrow |
| `query` | The current search in the form the board expects: `{prefix}` or `{from, to}`. `null` when there is none |
| `searchId` | A number increased with every new search. Each request remembers the value it started with, and its answer is ignored if a newer search has begun since |
| `page` | The number of the page being shown, counting from 0 |
| `cursors` | `cursors[i]` is the row that produces page `i`; `null` for page 0. Keeping all of them is what makes "previous" possible: going back is asking again with an earlier cursor |
| `starts` | `starts[i]` is how many rows come before page `i`, used for the "Rows 201 to 400" text |
| `total` | The number of rows in the whole search, from `/api/count`; `null` until it arrives |
| `pageRows`, `pageMore`, `pageError`, `pageLoaded` | The rows of the page being shown, in the order the board sent them; whether more follow; an error text; and whether the page has arrived |
| `sortColumn`, `sortDescending` | The column the rows on screen are sorted by (`-1` for none) and the direction |
| `dbVersion` | The database version from `/api/status`, added to every data request as `v` so the browser can cache the answers |

### Script functions

- **`element(id)`** is shorthand for looking up an element by its id.
- **`setStatus(text)`** shows a message in the status line. It uses `textContent`, so the text is displayed literally and is never interpreted as HTML.
- **`twoDigits(number)`** formats 7 as `07`.
- **`showUptime(seconds)`** shows seconds as hours, minutes and seconds, for example 3725 as `1:02:05`.
- **`showProblem(text)`** shows or hides the problem line.
- **`readAddress()`** turns the page's address into a search, or `null`. If only one of `from` and `to` is present, the other is taken as empty.
- **`requestParams()`** builds the parameters every data request shares: the search and the database version.
- **`fetchJson(url)`** fetches an address and reads the answer as JSON. The board sends errors as JSON too; if the status says failure but the document has no `error` member, one is added.
- **`startSearch(newQuery)`** begins a search: it resets the paging state, shows or hides the results area, writes the search into the form fields, and then calls `load()` and `loadTotal()`. The rows are requested first, so they arrive before the count.
- **`load()`** fetches the current page of rows.
  - The cursor for the page is added to the request. A row is `[year, state, name, ...]`, so the cursor's name is at index 2, its year at index 0 and its state at index 1.
  - Both buttons are disabled while the request runs, so a second click cannot start another request.
  - If the board cannot be reached, or the answer is not valid JSON, the status line reports the failure and "previous" is enabled again so the user is not stuck.
- **`loadTotal()`** fetches the row count for the search. A failure is ignored: the status line then simply shows no total.
- **`show(data)`** takes in one answer: it stores the rows, records the cursor and starting count for the next page if more rows follow, then calls `render()` and `showStatus()` and sets the buttons. "previous" is enabled on every page but the first, "next" only if more rows exist.
- **`showStatus()`** writes the status line: an error if there was one, "No records found" for an empty result, otherwise "Rows 201 to 400 of 1234". Until the total arrives the "of" part is left out, and the last page is marked "(end)".
- **`compareValues(a, b)`** compares two cell values for sorting: numbers numerically, everything else as text without regard to case.
- **`render()`** draws the table.
  - It sorts a copy of the rows if a sort column is set, and redraws the headings with an up or down arrow on that column.
  - Cells are created through the DOM and filled with `textContent`, so a name containing `<` or `&` is shown as it is.
  - Columns 5 and 6 (ratio and rate per 100k) are shown with exactly two decimals. The board sends them without trailing zeros, so `80.8` arrives and is shown as `80.80`. A type check guards against `null`, which the board sends for a value that cannot be written as a number.
- **`search(newQuery)`** is what the forms call: it puts the search into the address bar and starts it.

### Sorting

Clicking a column heading sorts the rows on screen by that column. A second click reverses the order and a third restores the order the board sent. The chosen sort is kept when moving to another page or starting another search.

Sorting rearranges only the rows currently shown, up to 200. It does not sort the whole result. Paging is unaffected by it: the cursor for the next page is always taken from the board's own order, not from what is on screen.

### Startup sequence

The script requests `/api/status` first. From it come the uptime, the database version that data requests should carry, and any problem to report. Then the search in the address, if there is one, is started. The promise chain is built so that this happens whether or not the status request succeeds: a failure only costs the uptime display, the problem line and the caching.

## Building, uploading and testing

Run these from the project folder.

| Command | Effect |
|---|---|
| `pio run -e Release` | Build the firmware |
| `pio run -e Release -t upload` | Build and upload the firmware |
| `pio run -e Release -t uploadfs` | Build and upload the filesystem image from `data/` |
| `pio device monitor` | Show the board's serial output |
| `python tools/rebuild_db.py` | Rebuild `data/babyname.db` from the original |
| `python tools/test_api.py [http://address]` | Check the addresses of a running board |
| `python tools/test_page.py [http://address]` | Check the page of a running board in a browser |

Both test scripts default to `http://esp32.local`. Passing the board's IP address avoids a name lookup of about a second per request on Windows. Both need only the Python standard library. On this PC, Python is the one that comes with PlatformIO: `%USERPROFILE%\.platformio\penv\Scripts\python.exe`.

**`tools/test_api.py`** fetches every page of a set of ranges and prefixes and compares the rows, and the counts, with the same queries run on `data/babyname.db`. It also checks the error answers, the caching headers, the file listing and download, and that nothing outside the files folder can be fetched. It expects `data/files/read.me` to be on the board. It makes 41 checks.

**`tools/test_page.py`** opens the page in Microsoft Edge or Google Chrome, running without a window, and looks at what a person would see. It checks the uptime display, a search given in the address, paging forward and back against the database, that a repeated page comes from the browser's cache, sorting, searching from both forms without a reload, an empty result, and the back button. It makes 33 checks. It drives the browser through its remote debugging interface, so nothing has to be installed.

Both passed in full against the firmware described here.

To replace the data: update the original database, run `tools/rebuild_db.py`, then upload the filesystem.

## Version control

The project is a Git repository. `include/Secrets.h` is excluded by `.gitignore`, as are PlatformIO's build folder, the tooling folder `.venv` and a saved `coredump.bin`. A fresh copy of the project needs a `Secrets.h` made from `Secrets.example.h` before it compiles.

Both database files are committed, since the project cannot be built into a working board without them.

## Reading a crash

There are two ways to find out where the firmware crashed. The second one, the saved crash dump, was tried on this board with a test firmware that wrote through a null pointer; the commands and the sample output below come from that test. The first is standard PlatformIO behaviour and was not tried here.

### While watching: the serial monitor

When the firmware crashes it prints the processor state and a backtrace of addresses to the serial port, then restarts. Built and monitored with the Debug environment, the addresses are translated into function names and line numbers:

```
pio run -e Debug -t upload
pio device monitor -e Debug
```

This only helps if the monitor was running at the moment of the crash.

### Afterwards: the saved crash dump

The framework also saves the state of every task to the `coredump` partition before restarting. The dump stays there until the next crash overwrites it, so it can be read later.

One-time setup, from the project folder (PowerShell). It installs Espressif's `esp-coredump` tool into a private folder, `.venv`, without touching PlatformIO's own Python:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m venv .venv
.venv\Scripts\python.exe -m pip install esp-coredump
```

To read a dump:

```powershell
# 1. Copy the partition from the board into a file.
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32 --port COM4 read_flash 0x3F0000 0x10000 coredump.bin

# 2. Decode it against the firmware that crashed.
.venv\Scripts\esp-coredump.exe --chip esp32 info_corefile --gdb "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32\bin\xtensa-esp32-elf-gdb.exe" --core coredump.bin --core-format raw .pio\build\Release\firmware.elf
```

The output names the crashed task and the cause, and lists the call stack with file names and line numbers. From the test:

```
Crashed task handle: 0x3ffb8188, name: 'loopTask'
exccause       0x1d (StoreProhibitedCause)
excvaddr       0x0
#0  0x400d14fe in faultyFunction (where=0x0) at src/main.cpp:10
#1  setup () at src/main.cpp:20
```

Things to know:

- **The `.elf` file must belong to the firmware that crashed.** `.pio\build\Release\firmware.elf` is overwritten by every build, so read the dump before rebuilding, or keep a copy of the file for firmware you leave running.
- **`0x3F0000` and `0x10000`** are the start and size of the `coredump` partition in `defpart.csv`. If the table changes, these change with it.
- **Reading the partition restarts the board.**
- **A partition with no dump in it** makes step 2 stop with `Core dump version "0xffff" is not supported!`. That means there has been no crash since the partition was last erased.
- **To clear a dump** after reading it: run the first command with `erase_region 0x3F0000 0x10000` in place of `read_flash 0x3F0000 0x10000 coredump.bin`.
- The tool can also read the board directly through `--port`, but that mode needs a full ESP-IDF installation, which this project does not use.

## Measured performance

Measured on the board during development, for the range `S` to `T` (1,553 rows).

| Stage of the project | Time for the full result |
|---|---|
| Original: one HTML page, index lookup per row | about 9.6 s |
| Table stored in name order, page cache enabled | about 1.4 s |
| One network write per chunk | about 0.9 to 1.2 s |
| Paged JSON (current) | about 0.1 to 0.2 s for the first 200 rows; about 1 s for all 8 pages |

Current figures on the board, averaged over 111 full 200-row pages:

| | Time |
|---|---|
| Database work per page | about 35 ms |
| Sending per page | about 23 ms (12 to 56 ms) |
| Whole response per page | about 76 ms |
| Count of 3,594 rows | about 92 ms |
| Count of the whole table (25,192 rows) | about 0.7 s |

A page takes the same time whether it is the first or a deep one.

### The `-O2` experiment

The Release firmware was built with the compiler's speed optimisation (`-O2`) in place of size optimisation (`-Os`) and measured the same way.

| | `-Os` (kept) | `-O2` |
|---|---|---|
| Database work per page | 35.4 ms | 34.5 ms |
| Count of 3,594 rows | 92.5 ms | 93.6 ms |
| Firmware size | 1,322,989 bytes | 1,445,301 bytes |

That is a gain of about 2.5% on rows, none on counting, for 122 KB more flash. It was not adopted, and `platformio.ini` is unchanged. The time per row is evidently not spent in code the compiler can speed up.

## Known issues

**Access and security**

- There is no login. Anyone on the network can query the database, read the device status and download everything in the `files` folder.
- The WiFi password is stored as plain text in `include/Secrets.h`.

**Reliability**

- The fallback access point is only started at boot. If the WiFi network disappears while the board is running, the board relies on the framework reconnecting by itself.
- The server answers one request at a time. A slow request delays everyone else. The largest single request is the count for a search that matches the whole table, about 0.7 s.
- Serial uploads were unreliable on this board earlier in development. At times the board did not enter download mode without the BOOT button held, and at other times it stayed in download mode and did not start. All recent uploads worked unattended; the cause was never identified.

**The page**

- Sorting applies to the rows on screen only, not to the whole result.
- The page needs JavaScript. Without it the forms submit normally but no results are shown.
- `tools/test_page.py` looks for the browser in the usual Windows install folders. Elsewhere its path has to be given as the second argument.

**Data**

- The database is read-only on the board. Any change means rebuilding it on a PC and uploading the whole filesystem.
- Case is ignored only for the letters A to Z. Accented and non-Latin letters are matched exactly.
- The two decimal columns are rounded to two places. The current data has no value with more.

**Files**

- Subfolders of `files` are not listed, and only a fixed list of file types gets a specific content type.

**Network**

- Looking up `esp32.local` takes about a second on Windows. The board's IP address is assigned by the router and changed during development (from `.167` to `.185`).

**Housekeeping**

- Query timing is printed to the serial port for every request (`LOG_QUERY_TIMING` is on).
- Reading a crash dump is a manual procedure, and the board gives no sign that a dump is waiting.

## Suggested improvements

In rough order of value.

1. **Give the board a fixed address.** Either fill in `STATIC_IP` and `STATIC_GATEWAY` in `include/Definitions.h` or reserve the address on the router. This avoids the name lookup and the changing address.
2. **Decide what the project is for.** Several larger choices depend on it:
   - If the board should change its own data (edit records, log values, accept input), the database must be opened for writing, power-loss safety starts to matter, and the caching rules change.
   - If it will be reachable by people you do not trust, it needs a login again and should not be exposed without encryption.
   - If it stays a read-only demonstration, the current design is sufficient.
3. **Sort the whole result on the board.** Sorting by another column across all pages would need the board to order the range itself, and paging by a different key. It is only worth doing if sorting the visible page proves too limited.
4. **Report a waiting crash dump.** The firmware could check the `coredump` partition at startup and say in `/api/status`, and on the page, that a crash was recorded.
5. **Turn the timing log off** when it is no longer needed, or enable it only in the Debug build.
6. **Handle a lost WiFi connection at run time**, falling back to the access point after a period without the network.
7. **Compress the page.** Embedding a gzip-compressed copy would cut the 7 KB transfer by more than half. The gain is small because the page is cached.
8. **Run the tests automatically.** Both test scripts need a board on the network; a single command that uploads, waits for the board and runs both would make that routine.
9. **Reconsider updates over WiFi.** They were added and removed during development because they need two firmware slots, which leaves about 1.1 MB for the filesystem. They would remove the dependence on the serial upload.

Done since the first version of this document: version control, a content-based cache label for the page, no automatic formatting of the filesystem, an automated browser test, a row total, searching without a reload, column sorting, the `-O2` measurement, and the crash-dump procedure.
