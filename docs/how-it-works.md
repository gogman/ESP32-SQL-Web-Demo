# SQLite_Web: how it works

SQLite_Web is firmware for an ESP32 board that serves a small web application. The application searches a SQLite database of baby names stored on the board's own flash memory and shows the results in a browser, 200 rows at a time.

This document describes how the program is put together, how the web page works, how to build, test and diagnose it, what is known to be wrong or fragile, and what could be improved next. It reflects the state of the project on 13 October 2026.

## Purpose

SQLite_Web is a read-only demonstration. The board never changes its data, and it is meant for a trusted network. Three choices in the design follow from that:

- The database is opened read-only, so a power loss cannot damage it and answers can be cached for as long as the database version stays the same.
- There is no login and no encryption.
- The data is replaced only by rebuilding the database on a PC and uploading the filesystem.

If the board is ever asked to write data, or is exposed to people who are not trusted, all three have to be revisited.

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
| `web/index.html` | The application page, built into the firmware compressed |
| `web/index.html.gz` | The compressed page, generated before every build; not in version control |
| `web/bootstrap.min.css` | Bootstrap 5.3.8, the stylesheet the page links to. Not uploaded itself; see `tools/pack_css.py` |
| `data/` | The filesystem image: `babyname.db` and `bootstrap.min.css.gz` |
| `db_backup/babyname.original.db` | The original database that `data/babyname.db` is built from |
| `tools/rebuild_db.py` | Builds `data/babyname.db` from the original |
| `tools/pack_page.py` | Compresses `web/index.html` into `web/index.html.gz`; PlatformIO runs it before every build |
| `tools/pack_css.py` | Compresses `web/bootstrap.min.css` into `data/bootstrap.min.css.gz` |
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
| `/bootstrap.min.css` | The Bootstrap stylesheet, gzip-compressed |
| anything else | `404`, or `405` for a method other than `GET` |

### `/api/names`

| Parameter | Meaning |
|---|---|
| `prefix` | Names starting with this text. Use this, or `from` and `to` |
| `from`, `to` | The first and last name of a range, both included |
| `after_name`, `after_year`, `after_state` | Optional. The last row of the previous page; the answer continues after it |
| `offset` | Optional, and only used when no cursor is given. The number of rows of the range before the page. Reaches any page directly, at a cost that grows with the number |
| `sort` | Optional. The column to sort the whole range by, as its position in a row: `0` year, `1` state, `2` name, `3` total babies, `4` primary sex, `5` ratio, `6` per 100k. Pages of a sorted range are found by `offset`; a cursor is ignored. Anything but `0` to `6` is answered 400 |
| `desc` | Optional, with `sort`. `1` puts the largest first |
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
| `crash_dump` | Whether a crash dump is waiting in the `coredump` partition; see [Reading a crash](#reading-a-crash) |
| `crash_task`, `crash_pc` | The task that crashed and the program address it crashed at; empty and `0x00000000` when no dump is waiting |
| `db_version` | The database file's size and modification time; changes when the database is replaced |
| `db_cache_kib` | The size of SQLite's page cache |
| `heap_free`, `psram_size`, `psram_free`, `flash_size` | Memory figures in bytes |

### `/bootstrap.min.css`

The page links to Bootstrap 5.3.8. The stylesheet is 232 KB, so it is not built into the firmware like the page. It is kept on the filesystem as `bootstrap.min.css.gz`, 31 KB, and sent exactly as stored with `Content-Encoding: gzip`; the browser unpacks it. No uncompressed copy is on the board, because every browser accepts gzip.

The page asks for it as `/bootstrap.min.css?v=5.3.8`. With the version in the address, the answer is marked `public, max-age=31536000, immutable`, so a browser fetches it once and does not ask again. If the file or the filesystem is missing, the answer is a 404 and the page still works, without Bootstrap's styling.

To change the version: replace `web/bootstrap.min.css`, run `python tools/pack_css.py`, change the version in the link in `web/index.html`, then upload both the filesystem and the firmware.

All of the page's styling comes from Bootstrap's classes; see [Styling](#styling).

This is the only file the board publishes. The database file is used by the firmware only and cannot be downloaded.

## The database

### Layout

`tools/rebuild_db.py` builds the database the board reads. Three properties of its layout matter, and the firmware's queries depend on all of them:

- **Rows are stored sorted by name.** The table is `WITHOUT ROWID` with the primary key `(name, year, state)`, so the rows themselves lie in that order. A range of names is one sequential read. The original layout needed a separate index lookup for every row, which cost about 5.8 ms per row on the board.
- **Names ignore case.** The `name` column uses the `NOCASE` collation, so `bob` finds `Bob`. This applies to the letters A to Z only.
- **Pages are 4096 bytes**, matching the block size of the filesystem.

### The queries

The firmware runs three queries, all compiled once when the database is opened.

Rows, continuing after a cursor:

```sql
Select year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state
from gendered_names
where name between ?1 and ?2 and (name, year, state) > (?3, ?4, ?5)
order by name, year, state limit ?6
```

`?1` and `?2` bound the names. `?3` to `?5` are the cursor: the key of the last row already shown. `?6` is the row limit.

Rows, by position:

```sql
Select year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state
from gendered_names
where name between ?1 and ?2
order by name, year, state limit ?3 offset ?4
```

This one reads and discards the first `?4` rows of the range before returning any. It exists for jumps to a page whose cursor is not known.

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

### Jumping to a page

A cursor only leads to the page after one already seen. To reach any other page, such as the last one, the board is asked by position with `offset`. The cost of that grows with the position, because the rows before the page are read and discarded: the last page of a 1,553-row range takes about as long as any other page, while the last page of the whole table (position 25,000) takes about 0.7 s.

The page therefore uses a cursor whenever it has one and a position only when it does not.

### Sorting on the board

With `sort`, the board orders the whole range by one column before cutting out a page. Rows with equal values are put in name order. Descending is exactly the ascending order backwards.

SQLite is not asked to do this. Both ways of having SQLite sort were tried on the board and neither is usable here:

- A query with `order by ... limit ...` makes SQLite sort in a temporary table. In this build of the library that table has a cache of less than one page, and sorting 330 rows took between 1 and 110 seconds depending on the direction.
- A query with `order by` and no limit uses SQLite's merge sorter. It took about 0.7 ms per row, which would be over 15 s for the whole table, on every page. Above 1 MB of data it also writes temporary files to the filesystem, and that crashed the firmware (a division by zero inside LittleFS).

So the firmware sorts by itself, in `db_mgr::getSortedPage()`:

1. **Load.** The whole range is read once in name order, with the ordinary position query and no limit. Every row is stored in memory as the same JSON text a normal page would contain, one after another.
2. **Sort.** A second, small table has one entry per row: where the row's text is, and its value in the chosen column. The value is read back out of the JSON text. Only this table is sorted; the text never moves.
3. **Serve.** A page is copied straight out of memory, walking the table forwards, or backwards for descending.

One range is kept at a time, until a different one is sorted. That makes the cost uneven:

| Request | Cost |
|---|---|
| First sorted request for a range | Reads every row: 0.3 s for 1,553 rows, 0.5 s for 3,594, about 3.1 s for the whole table |
| A different column, same range | Sorts again only: 0.2 to 0.6 s for the whole table |
| Another page, or the other direction | 0.05 to 0.1 s, wherever the page is |

The memory comes from PSRAM: about 62 bytes per row, 1.6 MB for the whole table, out of 4 MB. If it cannot be had, the request is answered with an error and nothing is kept. A board without PSRAM sorts ranges of up to 48 KiB of row text only.

Two simplifications, neither visible with the current data: the two decimal columns are compared as they are shown, rounded to two places, and the text columns (state and primary sex) are compared by their first eight bytes.

### Output

Rows are written as JSON directly into a fixed 1400-byte buffer inside the database object. When the buffer is full it is handed to the web handler, which sends it, and filling starts again. A query therefore allocates no memory of its own, and a result of any size can be streamed. Numbers are formatted by small hand-written routines, not `printf`. The two decimal columns are rounded to two places.

## Sending responses

Results are sent with HTTP chunked encoding, because their length is not known in advance. The stock web server writes each chunk as three separate network packets (length, data, line break). `ChunkServer` in `include/chunk_server.h` assembles the three parts in one buffer and writes them once, so a chunk is one packet. A full 1400-byte chunk with its framing still fits in a single TCP packet.

The HTTP status and headers of `/api/names` are sent together with the first chunk, not before the query starts. That is what allows a query that fails immediately to be answered with a proper `500`.

## Caching

- **The page** is sent with an `ETag` and `Cache-Control: no-cache`. The browser keeps its copy and asks each time whether it has changed; if not, the board answers `304 Not Modified` with no body. The `ETag` is computed at startup from the page itself (the length of the compressed page and an FNV-1a checksum of it), so it changes exactly when the page changes.
- **Rows and counts** requested with the `v` parameter are sent with `Cache-Control: private, max-age=86400`, so the browser may reuse them for a day. The value of `v` is the database version. When a new database is uploaded the version changes, the request addresses change with it, and old cached answers are not reused.
- **Status and errors** are never cached.

## The web page

`web/index.html` is the whole user interface. It is not stored on the board's filesystem: the build embeds it into the firmware, compressed, and `src/web_handlers.cpp` sends it for `/` and `/query_db`.

Before every build PlatformIO runs `tools/pack_page.py` (`extra_scripts` in `platformio.ini`), which gzip-compresses the page into `web/index.html.gz`; `board_build.embed_files` then copies that file into the firmware. The board sends it exactly as stored, with `Content-Encoding: gzip`, and the browser unpacks it: about 4.9 KB on the wire instead of 14.3 KB, and 9 KB less firmware. Every browser accepts gzip, so no uncompressed copy is kept. A tool that does not unpack gzip, such as `curl` without `--compressed`, receives the compressed bytes. Edit `index.html` only; the `.gz` file is regenerated and is not in version control. After changing the page, the firmware has to be rebuilt and uploaded; uploading the filesystem does not update it.

The file itself contains no comments, to keep it small on the wire. This section is its documentation.

### How a search works

The page is both the search form and the results. A search never reloads it:

- Submitting either form is intercepted by the script. The script writes the search into the address bar (for example `/?prefix=bob` or `/?from=Bob&to=Bobby`) with `history.pushState`, and loads the results.
- Opening such an address directly, or reloading the page, runs the same search from the address.
- The browser's back and forward buttons step through earlier searches without reloading.

Because the address always describes the search, results can be bookmarked and shared.

### Structure

- **Header.** A coloured bar with the title, the uptime and the theme switcher. A `span` with the id `uptime` shows `...` until the script fills it in from `/api/status`. The switcher is three radio buttons named `theme`, `themeLight`, `themeDark` and `themeAuto`, drawn as a button group of three icons from Bootstrap Icons 1.13.2: `bi-sun` for light, `bi-moon` for dark and `bi-display` for auto. The icons are written into the page as inline SVG, so no icon font or extra file is loaded; they take their colour from the button (`fill='currentColor'`). Each button has a tooltip and a hidden text label for screen readers.
- **Problem line.** A red alert box with the id `problem`, hidden unless the board reports that the filesystem is not mounted or the database is not open.
- **Crash notice.** A yellow alert box with the id `crash`, shown when the board reports a waiting crash dump.
- **Two forms**, `prefixForm` and `rangeForm`, together in one card. They still declare method `GET` and action `/`, which is what they would do without the script, but the script handles their submission. The `id` attributes of the fields let the script read them and write the current search back.
- **Results area.** A `div` with the id `results`, hidden while there is no search. The table inside is hidden separately, so an empty result shows just the status line. The table heading (`head`) is clickable for sorting. Above and below the table is a pager, `pagerTop` and `pagerBottom`: two empty lists that the script fills in.

### Styling

The look comes from Bootstrap's classes in the markup. The page has two style rules of its own. One lines the icons up with the text, as the Bootstrap Icons stylesheet would (`vertical-align: -0.125em`). The other is for the table heading: a pointer cursor, which Bootstrap has no class for, and Bootstrap's "primary subtle" colours, set through the table's colour variables. Bootstrap's own `table-primary` would look the same in light mode but stays light blue in dark mode.

| Part | Bootstrap classes |
|---|---|
| Page width and margins | `container` |
| Header bar | `bg-primary`, `text-white`, flex utilities |
| Theme switcher | `btn-group btn-group-sm`, `btn-check` radio buttons with `btn btn-outline-light` labels, `visually-hidden` for the text labels |
| Problem line | `alert alert-danger` |
| Crash notice | `alert alert-warning` |
| Search forms | `card`, the grid (`row`, `col-sm`), `form-control`, `btn btn-primary` |
| Table | `table table-sm table-striped table-hover table-bordered` inside `table-responsive`, which lets a wide table scroll sideways on a narrow screen |
| Pagers | `pagination` with `page-item` and `page-link`; `active` marks the current page and `disabled` an unavailable item |

Number cells are right-aligned: `render()` gives every cell holding a number the class `text-end`.

Two things to keep in mind when changing the markup:

- The script hides elements with the `hidden` attribute. Bootstrap's display classes such as `d-flex` override that attribute, so they must not be put on `problem`, `crash`, `results` or `table`.
- If the stylesheet cannot be loaded, the page still works, unstyled.

### Script variables

| Variable | Purpose |
|---|---|
| `COLUMNS` | The column headings, used to redraw them with or without a sort arrow |
| `query` | The current search in the form the board expects: `{prefix}` or `{from, to}`. `null` when there is none |
| `searchId` | A number increased with every new search. Each request remembers the value it started with, and its answer is ignored if a newer search has begun since |
| `page` | The number of the page being shown, counting from 0 |
| `cursors` | `cursors[i]` is the row that produces page `i`; `null` for page 0. Keeping all of them is what makes "previous" possible: going back is asking again with an earlier cursor |
| `starts` | `starts[i]` is how many rows come before page `i`, used for the "Rows 201 to 400" text and as the position when a page is requested without a cursor |
| `pageSize` | How many rows the board puts on a full page. Learned from the first page when more rows follow; `0` until then |
| `total` | The number of rows in the whole search, from `/api/count`; `null` until it arrives |
| `pageRows`, `pageMore`, `pageError`, `pageLoaded` | The rows of the page being shown, in the order the board sent them; whether more follow; an error text; and whether the page has arrived |
| `sortColumn`, `sortDescending` | The column the search is sorted by (`-1` for none, which means name order) and the direction |
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
  - If the cursor for the page is known, it is added to the request. A row is `[year, state, name, ...]`, so the cursor's name is at index 2, its year at index 0 and its state at index 1.
  - If it is not known, which happens after a jump, the page is requested by position: `offset` is the page number times `pageSize`.
  - Everything in both pagers is disabled while the request runs, so a second click cannot start another request.
  - If the board cannot be reached, or the answer is not valid JSON, the status line reports the failure and "first" and "previous" are enabled again so the user is not stuck.
- **`loadTotal()`** fetches the row count for the search. A failure is ignored: the status line then simply shows no total, and "last" stays unavailable.
- **`show(data)`** takes in one answer: it stores the rows, records the page size (on the first page), the cursor and the starting count for the next page if more rows follow, then calls `render()`, `showStatus()` and `showPager()`.
- **`lastPage()`** works out the number of the last page from the total and the page size, or returns `-1` while either is unknown.
- **`showPager(back, ready)`** rebuilds both pagers; see [Navigation](#navigation). `back` allows "First" and "Previous", `ready` allows everything else that applies. Both are false while a page loads, and only `back` is true after a failed request.
- **`addPageItem(list, label, target, enabled, name)`** adds one item to a pager. The current page becomes a highlighted label; every other item becomes a button that carries its target in `data-page`.
- **`pagerClick(event)`** handles a click anywhere in a pager: it reads the target from the button, sets `page` and calls `load()`.
- **`showStatus()`** writes the status line: an error if there was one, "No records found" for an empty result, otherwise "Rows 201 to 400 of 1234". Until the total arrives the "of" part is left out, and the last page is marked "(end)".
- **`showSortArrow()`** redraws the headings, with an up or down arrow on the column sorted by.
- **`render()`** draws the table.
  - The rows are shown in the order the board sent them; the page itself sorts nothing.
  - Cells are created through the DOM and filled with `textContent`, so a name containing `<` or `&` is shown as it is.
  - Columns 5 and 6 (ratio and rate per 100k) are shown with exactly two decimals. The board sends them without trailing zeros, so `80.8` arrives and is shown as `80.80`. A type check guards against `null`, which the board sends for a value that cannot be written as a number.
- **`search(newQuery)`** is what the forms call: it puts the search into the address bar and starts it.

### Light and dark theme

Bootstrap 5.3 draws every component in a light or a dark version, chosen by the `data-bs-theme` attribute on the `html` element. The switcher in the header sets it:

| Choice | Theme |
|---|---|
| Light | Always light |
| Dark | Always dark |
| Auto | Follows the system setting (`prefers-color-scheme`), and changes with it while the page is open. The default |

The choice is kept in the browser's `localStorage` under `theme`, so it survives a reload. If storage is not available, as in some private windows, the page falls back to Auto.

The two functions that do this, `storedTheme()` and `applyTheme(choice)`, sit in a small script in the page's `head`, which runs before anything is drawn. That way a page set to dark never shows a flash of light while it loads. The rest, marking the current choice and reacting to clicks and to system changes, is in the main script; its variable `themeChoice` holds the current choice.

### Navigation

The same pager is shown above and below the table, built from Bootstrap's pagination component. Its items, from left to right:

| Item | Does | Available |
|---|---|---|
| First | Goes to the first page | On every page but the first |
| Previous | Goes back one page | On every page but the first |
| Page numbers | Go to that page. The current page is highlighted, with up to two pages on either side (`PAGES_AROUND`) | Once the total has arrived; until then only the current page's number is shown |
| Next | Goes forward one page | When more rows follow |
| Last | Goes to the last page | When more rows follow and the total has arrived |

Each item only changes `page` and calls `load()`. A page number has a cursor if that page's predecessor was visited; otherwise the page is fetched by position. "next" always has a cursor, saved when the current page arrived. "last" usually has none, so that page is fetched by position, and so is "previous" from there. Pages reached that way record their own cursor for the page after them, so moving forward again uses cursors.

"last" depends on the total because the number of the last page is worked out from it. When the result fits on one page, both pagers are hidden.

### Sorting

Clicking a column heading sorts the whole search by that column, not just the rows on screen: the search starts again from its first page, with `sort` in every request. A second click reverses the order and a third returns to name order. The chosen sort is kept when starting another search.

While a sort is on, pages are requested by position (`offset`) and cursors are not used, because a cursor only means something in name order. The first sorted page of a large search takes noticeably longer than the ones after it; see [Sorting on the board](#sorting-on-the-board).

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

**`tools/test_api.py`** fetches every page of a set of ranges and prefixes and compares the rows, and the counts, with the same queries run on `data/babyname.db`. It also checks the error answers, the caching headers, that the page arrives compressed and unpacks to `web/index.html`, that the stylesheet arrives compressed and unpacks to `web/bootstrap.min.css`, that the database file cannot be fetched by address, pages requested by position, including the last page of the whole table, and sorted pages: three ranges by every column in both directions, and the whole table, each compared with the same sort done by SQLite on the PC. It makes 60 checks.

**`tools/test_page.py`** opens the page in Microsoft Edge or Google Chrome, running without a window, and looks at what a person would see. It checks the uptime display, that the Bootstrap stylesheet is applied, the light and dark theme (Auto following a system setting the browser is told to pretend, both fixed choices, and a choice surviving a reload), that the crash notice matches what the board reports, a search given in the address, paging forward and back against the database, that a repeated page comes from the browser's cache, the first and last buttons, the page numbers and both pagers, sorting the whole search by a column in both directions and across pages and searches, searching from both forms without a reload, an empty result, and the back button. It makes 61 checks. It drives the browser through its remote debugging interface, so nothing has to be installed.

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

The framework also saves the state of every task to the `coredump` partition before restarting. The dump stays there until it is erased or the next crash overwrites it, so it can be read later.

The board says when a dump is waiting. At startup the firmware checks the partition for a complete dump and reads from it which task crashed and at which address. Three places then show it:

- the serial port, as one line at startup;
- `/api/status`, as `"crash_dump":true` with `crash_task` and `crash_pc`;
- the page, as a yellow notice such as "The firmware crashed at some point and saved a crash dump (task loopTask at 0x400d14fb)."

The notice means a crash happened at some point, not that it just happened: it stays until the dump is erased (see the end of this section). `tools/test_api.py` also prints a note when a dump is waiting.

This was tested by loading a test firmware that writes through a null pointer, then the real firmware again. Status and page reported task `loopTask`; after the partition was erased, both went back to reporting nothing.

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
- **To clear a dump** after reading it, which also removes the notice on the page: run the first command with `erase_region 0x3F0000 0x10000` in place of `read_flash 0x3F0000 0x10000 coredump.bin`.
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
| Last page of the whole table, by position | about 0.7 s, measured from the PC |
| First sorted page of the whole table | about 3.1 s, measured from the PC |
| Any later sorted page of the whole table, including the last | 0.05 to 0.1 s, measured from the PC |
| The whole table sorted by a different column | 0.2 to 0.6 s, measured from the PC |
| First sorted page of `S`..`T` (1,553 rows) / `A`..`B` (3,594 rows) | about 0.3 s / 0.5 s, measured from the PC |

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

- There is no login, by design (see [Purpose](#purpose)). Anyone on the network can query the database and read the device status, so the board belongs on a trusted network only.
- The WiFi password is stored as plain text in `include/Secrets.h`.

**Reliability**

- The fallback access point is only started at boot. If the WiFi network disappears while the board is running, the board relies on the framework reconnecting by itself.
- The server answers one request at a time. A slow request delays everyone else. The largest single request is the first sorted page of a search that matches the whole table, about 3 s. The count for such a search, and a jump to its last page in name order, take about 0.7 s each.
- Serial uploads were unreliable on this board earlier in development. At times the board did not enter download mode without the BOOT button held, and at other times it stayed in download mode and did not start. All recent uploads worked unattended; the cause was never identified.

**The page**

- The sort order is not part of the address, so a bookmarked or reloaded search comes back in name order.
- A sorted search keeps a copy of its rows in the board's memory until a different search is sorted, up to 1.6 MB for the whole table.
- The page needs JavaScript. Without it the forms submit normally but no results are shown.
- `tools/test_page.py` looks for the browser in the usual Windows install folders. Elsewhere its path has to be given as the second argument.

**Data**

- The database is read-only on the board, by design (see [Purpose](#purpose)). Any change means rebuilding it on a PC and uploading the whole filesystem.
- Case is ignored only for the letters A to Z. Accented and non-Latin letters are matched exactly.
- The two decimal columns are rounded to two places. The current data has no value with more.

**Network**

- Looking up `esp32.local` takes about a second on Windows. The board's IP address is assigned by the router and changed during development (from `.167` to `.185`).

**Housekeeping**

- Query timing is printed to the serial port for every request (`LOG_QUERY_TIMING` is on).
- Reading a crash dump is a manual procedure. The board reports that a dump is waiting, but the dump can only be read and erased over the serial cable.

## Suggested improvements

In rough order of value.

1. **Give the board a fixed address.** Either fill in `STATIC_IP` and `STATIC_GATEWAY` in `include/Definitions.h` or reserve the address on the router. This avoids the name lookup and the changing address.
2. **Turn the timing log off** when it is no longer needed, or enable it only in the Debug build.
3. **Handle a lost WiFi connection at run time**, falling back to the access point after a period without the network.
4. **Run the tests automatically.** Both test scripts need a board on the network; a single command that uploads, waits for the board and runs both would make that routine.
5. **Reconsider updates over WiFi.** They were added and removed during development because they need two firmware slots, which leaves about 1.1 MB for the filesystem. They would remove the dependence on the serial upload.

Not on this list, because the project is a read-only demonstration: writing to the database from the board, a login, and encryption. See [Purpose](#purpose).

Done since the first version of this document: version control, a content-based cache label for the page, no automatic formatting of the filesystem, an automated browser test, a row total, searching without a reload, column sorting (now of the whole search, on the board), first and last buttons, removal of the file listing, the Bootstrap stylesheet and pagination, the crash-dump notice, the compressed page, the `-O2` measurement, and the crash-dump procedure.
