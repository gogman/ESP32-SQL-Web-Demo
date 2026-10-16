# SQLite_Web: how it works

SQLite_Web is firmware for an ESP32 board that serves a small web application. The application searches a SQLite database of baby names stored on the board's own flash memory and shows the results in a browser, 25 rows at a time by default.

This document describes how the program is put together, how the web page works, how to build, test and diagnose it, what is known to be wrong or fragile, and what could be improved next. It reflects the state of the project on 16 October 2026.

## Purpose

SQLite_Web is a read-only demonstration. The board never changes its data, and it is meant for a trusted network. Three choices in the design follow from that:

- The database is opened read-only, so a power loss cannot damage it and answers can be cached for as long as the database version stays the same.
- There is no login and no encryption. This includes the setup page, where the network settings are changed; it is not password protected for now.
- The data is replaced only by rebuilding the database on a PC and uploading the filesystem.

If the board is ever asked to write data, or is exposed to people who are not trusted, all three have to be revisited.

## At a glance

| | |
|---|---|
| Board | Freenove ESP32-WROVER, 4 MB flash, 4 MB PSRAM |
| Framework | Arduino core 2.0.17 (PlatformIO package 4.20017), built with PlatformIO (platform 7.1.3), from the project's own packages folder |
| Database | SQLite through the Sqlite3Esp32 library 2.5.0 (SQLite 3.43.2) |
| Data | One table, `gendered_names`, 25,192 rows, 835,584 bytes |
| Filesystem | LittleFS, 2048 KB partition |
| Address | `http://esp32-sql.local` (from the host name, which can be changed on the setup page), or the board's IP address |
| Setup page | `/admin/setup/`: host name, network mode, WiFi network and password, access point name, channel, maximum clients, DNS interval, country and transmit power, all kept in NVS |
| Firmware size | Release about 67% and Debug about 72% of the 1920 KB firmware partition |
| Version control | Git repository in the project folder |

## Project layout

| Path | What it holds |
|---|---|
| `src/main.cpp` | Entry point: `setup()` and `loop()` |
| `src/settings.cpp`, `include/settings.h` | The network settings: defaults, checks, and keeping them in NVS |
| `src/network.cpp`, `include/network.h` | WiFi, the access point, the `.local` name, the restart after saving settings |
| `src/db_mgr.cpp`, `include/db_mgr.h` | The database: opening it, producing pages of rows as JSON, counting rows |
| `src/web_handlers.cpp`, `include/web_handlers.h` | Every address the web server answers |
| `include/chunk_server.h` | A web server class that sends streamed responses efficiently |
| `include/Definitions.h` | All settings |
| `include/Secrets.h` | WiFi name and passwords; not kept in version control. `Secrets.example.h` is the template |
| `web/index.html` | The application page, built into the firmware compressed |
| `web/admin.html` | The setup page (`/admin/setup/`), built into the firmware the same way |
| `web/favicon.ico` | The page's icon: the Espressif logo at 16, 32 and 48 pixels, built into the firmware |
| `web/index.html.gz`, `web/admin.html.gz` | The compressed pages, generated before every build; not in version control |
| `web/bootstrap.min.css` | Bootstrap 5.3.8 with one local change (see below), the stylesheet the page links to. Not uploaded itself; see `tools/pack_css.py` |
| `data/` | The filesystem image: `babyname.db` and `bootstrap.min.css.gz` |
| `tools/rebuild_db.py` | Builds `data/babyname.db` from a database in the original layout |
| `tools/pack_page.py` | Compresses `web/index.html` and `web/admin.html` into `.html.gz` files; PlatformIO runs it before every build |
| `tools/pack_css.py` | Compresses `web/bootstrap.min.css` into `data/bootstrap.min.css.gz` |
| `tools/test_api.py` | Checks the addresses of a running board against the database file |
| `tools/test_page.py` | Checks the page of a running board in a real browser |
| `tools/test_setup.py` | Checks the setup page and the settings interface, including a save and the restart that follows |
| `tools/run_tests.py` | Builds and uploads the firmware, waits for the board and runs the three tests |
| `tools/board_tests.py` | The same tests as pytest tests, for VS Code's Testing panel |
| `tools/requirements.txt` | The Python packages for the Testing panel (pytest) |
| `pytest.ini` | Tells pytest where the tests are |
| `defpart.csv` | The flash partition table |
| `platformio.ini` | Build, upload and monitor settings |

The C++ files, the Python tools and the two configuration files carry detailed comments. The web page carries none; its explanation is in [The web page](#the-web-page) below.

## Flash layout

`defpart.csv` divides the 4 MB chip as follows.

| Partition | Start | Size | Purpose |
|---|---|---|---|
| `nvs` | 0x9000 | 20 KB | System settings such as radio calibration, and the network settings from the setup page (namespace `netcfg`) |
| (boot data) | 0xE000 | 8 KB | Written by the upload tool with every firmware upload; not a partition |
| `factory` | 0x10000 | 1920 KB | The firmware, a single copy |
| `spiffs` | 0x1F0000 | 2048 KB | The LittleFS filesystem (the subtype name is historical) |
| `coredump` | 0x3F0000 | 64 KB | Crash dumps saved by the framework |

Changing this table moves the partitions. Afterwards both the firmware and the filesystem image must be uploaded again, or the board finds no filesystem where it expects one.

## Startup

`setup()` in `src/main.cpp` brings the board up in this order:

1. **LED and serial port.** The serial port is started in the Debug build only, at 115200 baud, so later steps can report progress; the Release build prints nothing of its own (see [Serial output](#serial-output)).
2. **Settings.** The settings are read from NVS; see [The setup page](#the-setup-page). A value that was never saved gets its default; if a saved value is no longer valid, all settings go back to their defaults except the access point name.
3. **WiFi.** The radio is set up for the chosen country and transmit power. In access point mode the board starts its access point and nothing else. Otherwise it tries the WiFi network from the settings for up to 20 seconds; if that fails it starts its access point instead. The access point has the name, channel and client limit from the settings (by default `ESP32 SQL` and six random hex digits, channel 1, 4 clients) and the password `AP_PASSWORD` from `Secrets.h`, and the application is reachable through it at `192.168.4.1`. A captive DNS server runs alongside it; see [The setup page](#the-setup-page).
4. **Filesystem.** LittleFS is mounted. It is never formatted by the firmware. If it cannot be mounted, the board says so on the page and in `/api/status` (and on the serial port in the Debug build), and carries on without it.
5. **Database.** `babyname.db` is opened read-only. With PSRAM present, SQLite gets a 1024 KiB page cache, which is larger than the whole database. If opening fails, the server still starts and every query answers with the error.
6. **Name on the network.** The board announces itself by mDNS under its host name in lower case, `esp32-sql.local` by default, and gives the router the same name when it asks for an address.
7. **Web server.** The handlers are registered and the server starts listening on port 80.

`loop()` then repeats three things: serve one waiting request, do network housekeeping, and sleep for a millisecond so the processor is not kept busy doing nothing.

Network housekeeping (`handleNetwork()`) first carries out a restart that saving new settings asked for (1.5 s after the save, `RESTART_DELAY_MS`), and lets the captive DNS server answer, if it runs, at the chosen interval. Apart from that it has two jobs, depending on the mode:

- **On the WiFi network**, it watches the connection. When it drops, the ESP32 tries to reconnect by itself, and a short drop is over within seconds. If the connection is still lost after one minute (`WIFI_LOST_TIMEOUT_MS`), the board gives up on the network and starts the fallback access point, just as at startup. Because the web server listens on every interface, it carries on unchanged at `192.168.4.1`.
- **With the fallback access point running**, every five minutes (`AP_RETRY_MS`) the board restarts to try the WiFi network again, unless somebody is connected to the access point. In access point mode, chosen on the setup page, there is no network to go back to, and the access point stays.

So after a lasting loss of the network, the board is reachable through its access point after a minute, and back on the network within about five minutes of the network returning.

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
| `/favicon.ico` | The page's icon, shown in the browser tab and in bookmarks |
| `/admin/setup/` | The setup page; see [The setup page](#the-setup-page). `/admin/setup` without the final slash is sent on to it (301) |
| `/api/settings` | The settings: `GET` reads them, and the choices the page offers, as JSON; `POST` saves them and restarts the board |
| any other address, access point running | Sent on (302) to `http://192.168.4.1/` when the request names another site; see the captive DNS server under [The setup page](#the-setup-page) |
| anything else | `404`, or `405` for a method other than `GET` |

### `/api/names`

| Parameter | Meaning |
|---|---|
| `startswith` | Names starting with this text. Use this, or `from` and `to` |
| `from`, `to` | The first and last name of a range, both included |
| `after_name`, `after_year`, `after_state` | Optional. The last row of the previous page; the answer continues after it |
| `offset` | Optional, and only used when no cursor is given. The number of rows of the range before the page. Reaches any page directly, at a cost that grows with the number |
| `sort` | Optional. The column to sort the whole range by, as its position in a row: `0` year, `1` state, `2` name, `3` total babies, `4` primary sex, `5` ratio, `6` per 100k. Pages of a sorted range are found by `offset`; a cursor is ignored. Anything but `0` to `6` is answered 400 |
| `desc` | Optional, with `sort`. `1` puts the largest first |
| `sex` | Optional. `M` or `F` (either case) keeps only rows with that primary sex; left out or empty, all rows. Anything else is answered 400 |
| `limit` | Optional. Rows per page, 1 to 200 (`DB_PAGE_ROWS`); 25 (`DB_DEFAULT_ROWS`) when left out. Anything else is answered 400. The page asks for 10, 25, 50, 100 or 200 |
| `v` | Optional. The database version from `/api/status`. Its presence makes the answer cacheable |

The answer has this form:

```json
{"rows":[[2015,"US","Bob",14,"M",100,0.4],[2015,"US","Bobbi",67,"F",100,1.8]],"more":false}
```

Each row is `[year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state]`. `more` is `true` when rows remain beyond this page. Names are matched without regard to case.

A request without a range is answered `400`. A database failure before any output is answered `500`. Both carry `{"error":"..."}`. If a failure happens after output has started, the document is still completed as valid JSON with an added `error` member.

### `/api/count`

Takes the same `startswith` or `from` and `to` parameters, `sex` and `v`, and answers `{"count":1234}`. Errors are reported as for `/api/names`.

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

One change is made to the stylesheet by hand: in the light theme, cards have the background `#e3e3e3` instead of the page's white. The `.card` rule sets `--bs-card-bg:#e3e3e3`, and an added rule right after it, `[data-bs-theme=dark] .card{--bs-card-bg:var(--bs-body-bg)}`, keeps the dark theme as Bootstrap has it. A new version of Bootstrap needs the same change made again.

The page asks for it as `/bootstrap.min.css?v=5.3.8-1`; the `-1` counts local changes, so a browser that kept the earlier file fetches the changed one. With the version in the address, the answer is marked `public, max-age=31536000, immutable`, so a browser fetches it once and does not ask again. If the file or the filesystem is missing, the answer is a 404 and the page still works, without Bootstrap's styling.

To change the version, or the stylesheet itself: replace `web/bootstrap.min.css`, run `python tools/pack_css.py`, change the version in the link in `web/index.html` (and in `tools/test_api.py`), then upload both the filesystem and the firmware.

All of the page's styling comes from Bootstrap's classes; see [Styling](#styling).

This is the only file the board publishes. The database file is used by the firmware only and cannot be downloaded.

## The setup page

`/admin/setup/` is a second page, built into the firmware like the application page (`web/admin.html`). Its settings are kept in NVS (the `nvs` partition, namespace `netcfg`), so they survive restarts and firmware and filesystem uploads. It is styled with Bootstrap and has the same header as the application page: the uptime clock (read from `/api/status`, counting on by itself, and started again after a restart) and the theme switcher, which shares its choice with the application page. It is not password protected (see [Purpose](#purpose)).

The page is static: it holds no values when it is sent. Its script reads them from `GET /api/settings`, together with the lists of countries, transmit power levels and DNS intervals, and fills in the fields. So the page can be compressed and cached like the application page, and it offers exactly the choices the board accepts, from the tables in `src/settings.cpp`.

| Field (form name) | NVS key | Default | Rules |
|---|---|---|---|
| Host name (`hostname`) | `hostname` | `ESP32-SQL` | 1 to 32 letters, digits and hyphens, not starting or ending with a hyphen |
| Network mode (`mode`) | `ap_mode` | Join a Wi-Fi network (`sta`) | `sta` (join the network; the access point is the fallback) or `ap` (access point only) |
| Network name (`sta_ssid`) | `wifi_ssid` | `WIFI_SSID` from `Secrets.h` | 1 to 32 bytes; may be empty in access point mode |
| Password (`sta_pass`, `sta_pass_confirm`) | `wifi_pass` | `WIFI_PASSWORD` from `Secrets.h` | 8 to 63 characters, typed twice. Both empty keeps the saved password |
| Open network (`sta_open`) | (an empty `wifi_pass`) | Ticked only if no password is saved | Ticked removes the password. Unticked with no password saved and none typed is refused in `sta` mode |
| Access point name (`ap_ssid`) | `ap_ssid` | `ESP32 SQL` and six random hex digits, e.g. `ESP32 SQL 3FA9C1` | 1 to 32 bytes |
| Wi-Fi channel (`wifi_channel`) | `channel` | 1 | 1 to 11 in the United States, Canada, Mexico, Taiwan and "World (safe mode)"; 1 to 13 elsewhere |
| Maximum clients (`max_clients`) | `max_clients` | 4 | 1 to 10, the most the ESP32's access point serves |
| DNS interval (`dns_interval`) | `dns_ms` | 10 ms | Every pass, 10, 25, 50 or 100 ms |
| Country (`country`) | `country` | United States (`US`) | One of 45 in `kWifiCountries` (`src/settings.cpp`): "World (safe mode)" (`01`) first, then the countries in alphabetical order of their codes. The default is set separately, as `DEFAULT_COUNTRY` in `include/settings.h` |
| Transmit power (`tx_power`) | `tx_power` | 19.5 dBm | One of 12 levels in `kTxPowerLevels`, from 19.5 down to -1 dBm, saved as the radio counts them (quarters of a dBm, e.g. 78; -1 dBm is -4) |

The random digits of the access point name come from `esp_random()`, which draws on the radio's noise, at the first startup. The name is saved at once, so it stays the same from then on; a name that changed at every restart could not be found again. The access point's password is `AP_PASSWORD` from `Secrets.h` and cannot be changed on the page.

The host name is used as typed for DHCP and mDNS, in lower case: `ESP32-SQL` answers at `http://esp32-sql.local`. The page shows the address as the name is typed.

**How the radio settings are used.** At startup, right after the WiFi mode is set, the board sets the country with `esp_wifi_set_country()` (its channel range, fixed rather than taken from nearby access points) and the transmit power with `WiFi.setTxPower()`. Both apply to the WiFi connection as well as to the access point. The channel and the client limit apply to the access point only; on a WiFi network the router decides the channel.

**The captive DNS server.** Whenever the access point runs (chosen, or as the fallback), a DNS server on the board answers every name with the board's address (`DNSServer` in `src/network.cpp`). A request that then arrives for another site, typically a phone checking for internet access such as `http://connectivitycheck.gstatic.com/generate_204`, is sent on to `http://192.168.4.1/`. The phone takes that as a "sign in to network" page and shows the application. Requests by IP address or by a `.local` name are served as usual. The DNS interval sets how often the server answers waiting questions; shorter makes pages start loading sooner on the access point, at the cost of more processor time.

**Reading.** `GET /api/settings` answers, shortened:

```json
{"hostname":"ESP32-SQL","network_name":"esp32-sql","mode":"sta","sta_ssid":"home","sta_password_set":true,
 "ap_ssid":"ESP32 SQL 3FA9C1","wifi_channel":1,"max_clients":4,"dns_interval":10,"country":"US","tx_power":78,
 "max_clients_limit":10,"countries":[{"code":"US","name":"United States","channels":11},...],
 "tx_powers":[{"value":78,"label":"19.5 dBm (maximum)"},...],"dns_intervals":[{"value":0,"label":"Every pass"},...]}
```

The WiFi password itself is never sent, only whether one is saved. Texts are escaped as JSON defines, so network names with quotes or backslashes come back exactly.

**Saving.** The page posts an ordinary form to `/api/settings` with the field names in the table. The board checks every field (`checkSettings()` in `src/settings.cpp`, and the password rules in `handleSettingsPost()`); a text longer than its limit is refused before it is copied, rather than cut short, and a number that is not made of digits is refused. An invalid setting is answered 400 with `{"error":"...","field":"..."}`, nothing is saved, and the page marks the field. Valid settings are saved and answered with `{"saved":true,"restarting":true,"network_name":"..."}`, and the board restarts 1.5 s later to apply them. The page checks the same rules itself before sending, so most mistakes never reach the board.

After saving, the page says where the board will be: in WiFi mode `http://<host name>.local/`, after which it waits for the board to answer again and says when it has restarted; in access point mode, the access point's name and `http://192.168.4.1/`.

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
  and (?7 = '' or primary_sex = ?7)
order by name, year, state limit ?6
```

`?1` and `?2` bound the names. `?3` to `?5` are the cursor: the key of the last row already shown. `?6` is the row limit. `?7` is the primary sex filter: `M`, `F`, or empty, in which case `?7 = ''` is true for every row and the filter switches itself off. One compiled query therefore serves both the filtered and the unfiltered case. The other two queries carry the same filter.

Rows, by position:

```sql
Select year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state
from gendered_names
where name between ?1 and ?2 and (?5 = '' or primary_sex = ?5)
order by name, year, state limit ?3 offset ?4
```

This one reads and discards the first `?4` rows of the range before returning any. It exists for jumps to a page whose cursor is not known.

Count:

```sql
Select count(*) from gendered_names
where name between ?1 and ?2 and (?3 = '' or primary_sex = ?3)
```

The filter has to look at each row's `primary_sex`, so it does not make a search faster: the range is walked as before and the rows of the other sex are passed over. Counting the whole table takes about 0.6 s either way.

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

Before every build PlatformIO runs `tools/pack_page.py` (`extra_scripts` in `platformio.ini`), which gzip-compresses the page into `web/index.html.gz`; `board_build.embed_files` then copies that file into the firmware, together with `web/favicon.ico`.

The icon is the Espressif logo (Espressif makes the ESP32), taken from Espressif's own GitHub organisation and converted to a three-size `.ico` file. The board sends it for `/favicon.ico`, which the page also names in a `<link rel='icon'>`, with `Cache-Control: public, max-age=604800`, so a browser asks for it again at most once a week. It is in the firmware rather than on the filesystem so that it shows even when the filesystem is missing. The board sends it exactly as stored, with `Content-Encoding: gzip`, and the browser unpacks it: about 4.9 KB on the wire instead of 14.3 KB, and 9 KB less firmware. Every browser accepts gzip, so no uncompressed copy is kept. A tool that does not unpack gzip, such as `curl` without `--compressed`, receives the compressed bytes. Edit `index.html` only; the `.gz` file is regenerated and is not in version control. After changing the page, the firmware has to be rebuilt and uploaded; uploading the filesystem does not update it.

The file itself contains no comments, to keep it small on the wire. This section is its documentation.

### How a search works

The page is both the search form and the results. A search never reloads it:

- Submitting either form is intercepted by the script. The script writes the search into the address bar (for example `/?startswith=bob` or `/?from=Bob&to=Bobby`) with `history.pushState`, and loads the results.
- Opening such an address directly, or reloading the page, runs the same search from the address.
- The browser's back and forward buttons step through earlier searches without reloading.

Because the address always describes the search, results can be bookmarked and shared.

### Structure

- **Header.** A coloured bar with the title, the uptime and the theme switcher. A `span` with the id `uptime` shows `...` until the script fills it in from `/api/status`. The switcher is three radio buttons named `theme`, `themeLight`, `themeDark` and `themeAuto`, drawn as a button group of three icons from Bootstrap Icons 1.13.2: `bi-sun` for light, `bi-moon` for dark and `bi-display` for auto. The icons are written into the page as inline SVG, so no icon font or extra file is loaded; they take their colour from the button (`fill='currentColor'`). Each button has a tooltip and a hidden text label for screen readers.
- **Problem line.** A red alert box with the id `problem`, hidden unless the board reports that the filesystem is not mounted or the database is not open.
- **Crash notice.** A yellow alert box with the id `crash`, shown when the board reports a waiting crash dump.
- **Two forms**, `prefixForm` and `rangeForm`, together in one card, with the primary sex filter (`sex`: All, Male, Female) below them. They still declare method `GET` and action `/`, which is what they would do without the script, but the script handles their submission. The `id` attributes of the fields let the script read them and write the current search back.
- **Results area.** A `div` with the id `results`, hidden while there is no search. The table inside is hidden separately, so an empty result shows just the status line. The table heading (`head`) is clickable for sorting. Above and below the table is a toolbar: a pager on the left, `pagerTop` and `pagerBottom`, two empty lists that the script fills in, and a "Rows per page" list on the right, `perPageTop` and `perPageBottom`.
- **Loading overlay.** A `div` with the id `loading` that covers the whole window while rows are on their way. In its centre is a small box with a spinner and "Loading..." below it. See "The loading overlay" below.

### Styling

The look comes from Bootstrap's classes in the markup. The page has its own style rules for three things. One lines the icons up with the text, as the Bootstrap Icons stylesheet would (`vertical-align: -0.125em`). The other is for the table heading: a pointer cursor, which Bootstrap has no class for, and Bootstrap's "primary subtle" colours, set through the table's colour variables. Bootstrap's own `table-primary` would look the same in light mode but stays light blue in dark mode. The third is the loading overlay, described below.

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
| Loading overlay | `position-fixed top-0 start-0 w-100 h-100` and flex centring for the cover; `bg-body text-body border rounded-3 shadow` for the box; `spinner-border text-primary` for the spinner |

Number cells are right-aligned: `render()` gives every cell holding a number the class `text-end`.

Two things to keep in mind when changing the markup:

- The script hides elements with the `hidden` attribute. Bootstrap's display classes such as `d-flex` override that attribute, so they must not be put on `problem`, `crash`, `results` or `table`. The loading overlay does use `d-flex`, so it is hidden with its own style rule instead.
- If the stylesheet cannot be loaded, the page still works, unstyled.

### Script variables

| Variable | Purpose |
|---|---|
| `COLUMNS` | The column headings, used to redraw them with or without a sort arrow |
| `query` | The current search in the form the board expects: `{startswith}` or `{from, to}`. `null` when there is none |
| `searchId` | A number increased with every new search. Each request remembers the value it started with, and its answer is ignored if a newer search has begun since |
| `page` | The number of the page being shown, counting from 0 |
| `cursors` | `cursors[i]` is the row that produces page `i`; `null` for page 0. Keeping all of them is what makes "previous" possible: going back is asking again with an earlier cursor |
| `starts` | `starts[i]` is how many rows come before page `i`, used for the "201 to 400" part of the status line and as the position when a page is requested without a cursor |
| `perPage` | Rows per page: 10, 25, 50, 100 or 200. 25 whenever the page is opened, then the user's choice until the page is closed or reloaded |
| `sexFilter` | The primary sex filter: `''`, `'M'` or `'F'`. Not remembered across visits |
| `total` | The number of rows in the whole search, from `/api/count`; `null` until it arrives |
| `pageRows`, `pageMore`, `pageError`, `pageLoaded` | The rows of the page being shown, in the order the board sent them; whether more follow; an error text; and whether the page has arrived |
| `sortColumn`, `sortDescending` | The column the search is sorted by (`-1` for none, which means name order) and the direction |
| `loadNumber` | Counts the requests made by `load()`, so that only the latest one hides the loading overlay |
| `dbVersion` | The database version from `/api/status`, added to every data request as `v` so the browser can cache the answers |

### Script functions

- **`element(id)`** is shorthand for looking up an element by its id.
- **`setStatus(text)`** shows a message in the status line. It uses `textContent`, so the text is displayed literally and is never interpreted as HTML.
- **`twoDigits(number)`** formats 7 as `07`.
- **`formatUptime(seconds)`** writes seconds as days, hours, minutes and seconds, for example 90061 as `1d 1:01:01` and 3725 as `0d 1:02:05`.
- **`startUptime(seconds)`** shows the uptime the board reported and from then on adds the time that has passed, once a second. The board is not asked again: its clock and the browser's run at the same speed, so counting on locally gives the same result without a request every second to a server that answers one request at a time. The time passed is measured with `performance.now()`, which is not affected when the computer's clock is set. A browser slows timers in a background tab, but each update is worked out afresh from the clock, so the display is right again as soon as the tab is shown.
- **`showProblem(text)`** shows or hides the problem line.
- **`showLoading(on)`** shows or hides the loading overlay.
- **`readAddress()`** turns the page's address into a search, or `null`. If only one of `from` and `to` is present, the other is taken as empty.
- **`requestParams()`** builds the parameters every data request shares: the search and the database version.
- **`fetchJson(url)`** fetches an address and reads the answer as JSON. The board sends errors as JSON too; if the status says failure but the document has no `error` member, one is added.
- **`startSearch(newQuery, startPage)`** begins a search: it resets the paging state and starts on `startPage`, or on the first page when that is left out, shows or hides the results area, writes the search into the form fields, and then calls `load()` and `loadTotal()`. The rows are requested first, so they arrive before the count.
- **`load()`** fetches the current page of rows.
  - If the cursor for the page is known, it is added to the request. A row is `[year, state, name, ...]`, so the cursor's name is at index 2, its year at index 0 and its state at index 1.
  - If it is not known, which happens after a jump, the page is requested by position: `offset` is the page number times `perPage`. Every request carries `limit`, set to `perPage`.
  - Everything in both pagers is disabled while the request runs, so a second click cannot start another request.
  - If the board cannot be reached, or the answer is not valid JSON, the status line reports the failure and "first" and "previous" are enabled again so the user is not stuck.
- **`loadTotal()`** fetches the row count for the search. A failure is ignored: the status line then simply shows no total, and "last" stays unavailable.
- **`show(data)`** takes in one answer: it stores the rows, records the page size (on the first page), the cursor and the starting count for the next page if more rows follow, then calls `render()`, `showStatus()` and `showPager()`.
- **`lastPage()`** works out the number of the last page from the total and the page size, or returns `-1` while either is unknown.
- **`showPager(back, ready)`** rebuilds both pagers; see [Navigation](#navigation). `back` allows "First" and "Previous", `ready` allows everything else that applies. Both are false while a page loads, and only `back` is true after a failed request.
- **`addPageItem(list, label, target, enabled, name)`** adds one item to a pager. The current page becomes a highlighted label; every other item becomes a button that carries its target in `data-page`.
- **`pagerClick(event)`** handles a click anywhere in a pager: it reads the target from the button, sets `page` and calls `load()`.
- **`showStatus()`** writes the status line: an error if there was one, "No records found" for an empty result, otherwise the page and the rows, as in "Page 2 of 7 (201 to 400 of 1234 records)". The number of pages is the total divided by the rows per page, rounded up. Until the total arrives both "of" parts are left out ("Page 2 (201 to 400 records)"), except on the last page, where the rows seen so far are the total.
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
| Page numbers | Go to that page. The current page is highlighted, with up to four pages on either side (`PAGES_AROUND`): 5 numbers on the first and last page, growing to 9 from the fifth page on and back to 5 towards the end. A search with fewer pages shows them all | Once the total has arrived; until then only the current page's number is shown |
| Next | Goes forward one page | When more rows follow |
| Last | Goes to the last page | When more rows follow and the total has arrived |

Each item only changes `page` and calls `load()`. A page number has a cursor if that page's predecessor was visited; otherwise the page is fetched by position. "next" always has a cursor, saved when the current page arrived. "last" usually has none, so that page is fetched by position, and so is "previous" from there. Pages reached that way record their own cursor for the page after them, so moving forward again uses cursors.

"last" depends on the total because the number of the last page is worked out from it. When the result fits on one page, both pagers are hidden.

### The loading overlay

`load()` shows the overlay each time it asks for a page of rows, and hides it when the answer, or a failure, has been handled. It is drawn over everything (`z-index` 1090, above Bootstrap's own layers), so the page cannot be clicked while it is up. The cover is black at 25 % opacity, which darkens the page slightly in either theme. The box in the middle uses Bootstrap's body colours and the spinner the primary colour, so both follow the light and dark theme.

It is hidden with `visibility: hidden` and `opacity: 0`, and shown by adding the class `show`. The fade-in starts only after 0.15 s and takes 0.15 s, so a page that comes from the browser's cache, or a fast answer, does not make it flash; hiding it is immediate. Each call of `load()` gets a number (`loadNumber`), and only the answer to the latest request hides the overlay, so a slow earlier answer cannot hide it while a newer request is still waiting. The total count, fetched alongside, does not show it.

### Rows per page and the primary sex filter

The two "Rows per page" lists, above and below the table, always show the same value; changing either one changes both. The choice starts the search again on the page that holds the first row that was showing: from page 6 at 10 rows (rows 51 to 60), 25 rows per page shows page 3 (rows 51 to 75). It is not remembered: the page always opens at 25, the same default the board uses for a request without `limit`.

The primary sex list in the search card keeps only male or only female rows, or all. Changing it starts the current search again; the choice then stays for the following searches, like the sort order. Both values go into every request as `limit` and `sex`, and the filter into the count as well, so the status line and the pager count only the rows that are shown.

### Sorting

Clicking a column heading sorts the whole search by that column, not just the rows on screen: the search starts again on the page that was showing, with `sort` in every request, so page 3 stays page 3 in the new order. A second click reverses the order and a third returns to name order. The chosen sort is kept when starting another search.

While a sort is on, pages are requested by position (`offset`) and cursors are not used, because a cursor only means something in name order. The first sorted page of a large search takes noticeably longer than the ones after it; see [Sorting on the board](#sorting-on-the-board).

### Startup sequence

The script requests `/api/status` first. From it come the uptime, the database version that data requests should carry, and any problem to report. Then the search in the address, if there is one, is started. The promise chain is built so that this happens whether or not the status request succeeds: a failure only costs the uptime display, the problem line and the caching.

## Building, uploading and testing

Run these from the project folder.

| Command | Effect |
|---|---|
| `pio run -e Release` | Build the firmware |
| `pio run -e Release -t upload` | Build and upload the firmware |
| `pio run -e Debug -t upload` | Build and upload the Debug firmware, the only one with serial output; see [Serial output](#serial-output) |
| `pio run -e Release -t uploadfs` | Build and upload the filesystem image from `data/` |
| `pio device monitor` | Show the board's serial output (Debug build) |
| `python tools/rebuild_db.py source.db` | Rebuild `data/babyname.db` from a database in the original layout |
| `python tools/test_api.py [http://address]` | Check the addresses of a running board |
| `python tools/test_page.py [http://address]` | Check the page of a running board in a browser |
| `python tools/test_setup.py [http://address] [--no-restart]` | Check the setup page and the settings; without `--no-restart` it saves changed settings and restores them, restarting the board twice |
| `python tools/run_tests.py [http://address]` | Build, upload, wait for the board, run the three tests |

The test scripts default to `http://esp32-sql.local`. Passing the board's IP address avoids a name lookup of about a second per request on Windows. Both need only the Python standard library. On this PC, Python is the one that comes with PlatformIO: `%USERPROFILE%\.platformio\penv\Scripts\python.exe`.

**`tools/run_tests.py`** does the whole routine in one command: it builds and uploads the firmware, waits until the board answers with the new firmware, and runs the three test scripts against it, stopping at the first step that fails. It ends with a summary, and its exit code is 0 only if everything passed. It recognises the new firmware by its uptime: a board that answers but has been running longer than the time since the upload has not restarted yet. Before testing it also stops if the board has no usable database, and points out a waiting crash dump. Given a name such as `esp32-sql.local`, it looks the name up once and runs the tests against the IP address the board reports, because the tests make hundreds of requests and a lookup on Windows takes up to a second and now and then fails. `tools/test_api.py` also tries a request again, up to three times, when it cannot reach the board at all. Options:

| Option | Effect |
|---|---|
| `--no-upload` | Test the firmware already on the board |
| `--uploadfs` | Also upload the filesystem image first, after changing the database or the stylesheet |
| `--env Debug` | Build another environment (default `Release`) |
| `--api-only`, `--page-only`, `--setup-only` | Run only one of the three tests |
| `--no-restart` | Run the setup test without saving, so the board is not restarted |

In VS Code the same runs are available as tasks (`.vscode/tasks.json`): Terminal > Run Task... offers "Board tests: upload firmware and run all", "... run all without uploading" and "... upload filesystem and firmware, run all", and "Tasks: Run Test Task" starts the first one. Each asks for the board's address. PlatformIO's own Test button is something else: it runs `pio test`, which builds C++ unit tests, and this project has none.

The same tests also appear in VS Code's Testing panel, through pytest and the Python extension: `tools/board_tests.py` holds four tests, "board is ready" (it answers, has its database, and has no crash dump waiting), "addresses" (runs `tools/test_api.py`), "page in browser" (runs `tools/test_page.py`) and "setup page" (runs `tools/test_setup.py`, which restarts the board twice). A failing test lists the checks that failed, and its output holds the script's full report. These tests do not build or upload anything; they test the firmware already on the board. `pytest.ini` tells pytest where they are, and `.vscode/settings.json` turns pytest on and selects the project's own Python environment. That environment is set up once:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m venv .venv
.venv\Scripts\python.exe -m pip install -r tools
equirements.txt
```

The board's address comes from the environment variable `BOARD_URL`, default `http://esp32-sql.local`. To use another, put a line such as `BOARD_URL=http://172.16.100.185` in a file named `.env` in the project folder, which the Python extension reads. From a terminal the same tests run with `.venv\Scripts\python.exe -m pytest`.

A full run takes about a minute and a half: 30 to 40 s to build and upload, under 10 s for the board to restart, and about 25 s for each test.

**`tools/test_api.py`** fetches every page of a set of ranges and prefixes and compares the rows, and the counts, with the same queries run on `data/babyname.db`. It also checks the error answers, the caching headers, that the page arrives compressed and unpacks to `web/index.html`, that the icon arrives unchanged, that the stylesheet arrives compressed and unpacks to `web/bootstrap.min.css`, that the database file cannot be fetched by address, pages requested by position, including the last page of the whole table, that a request without `limit` gets 25 rows, every combination of 25 or 200 rows per page with no filter, male and female on two ranges, invalid values for both, and sorted pages: three ranges by every column in both directions, and the whole table, each compared with the same sort done by SQLite on the PC. It makes 86 checks.

**`tools/test_page.py`** opens the page in Microsoft Edge or Google Chrome, running without a window, and looks at what a person would see. It checks the uptime display (its format, that it counts on by itself, and the conversion into days, hours, minutes and seconds), that the Bootstrap stylesheet is applied, the light and dark theme (Auto following a system setting the browser is told to pretend, both fixed choices, and a choice surviving a reload), that the crash notice matches what the board reports, a search given in the address, paging forward and back against the database, that a repeated page comes from the browser's cache, the first and last buttons, the page numbers and both pagers, sorting the whole search by a column in both directions and across pages and searches, searching from both forms without a reload, an empty result, the back button, and the rows per page with the primary sex filter, including that the page opens at 25 rows again after a reload, and the loading overlay: shown at once by a search and gone when the rows are there, covering the window with its box centred both ways, the darkened background, the text, and its colours in both themes, the page numbers around the current page (5 at either end, 9 in the middle), and that changing the sort or the rows per page keeps the current page. It makes 93 checks. It drives the browser through its remote debugging interface, so nothing has to be installed.

**`tools/test_setup.py`** checks the setup page's address and its redirect, that the page arrives compressed and unpacks to `web/admin.html` and is answered 304 when asked again with its ETag, what `GET /api/settings` returns (the eleven values and the three lists, and never the WiFi password), and that 24 kinds of invalid settings are each refused with the right field named, nothing saved and no restart. In the browser it checks the uptime clock and the theme switcher, that the page shows the saved settings and the board's lists with no template placeholder left, the address as the host name is typed, the channel limit following the country, the open-network box switching the password fields off, and its own checks of the fields. Then it saves a changed host name, channel, client limit, DNS interval, country and transmit power through the page, waits for the restart, checks that they were kept and everything else, including the WiFi password, is unchanged, and saves the original settings back. The mode and the WiFi network are not changed, so the board stays on the same network.

All three passed in full against the firmware described here, on 16 October 2026; the setup test makes 67 checks, and its save and restore restarted the board in about 6 seconds each.

To replace the data: run `tools/rebuild_db.py` on a database in the original layout, then upload the filesystem. The original the current data was built from is no longer kept in the project, but it is in the repository's history: `git show c60ca07:db_backup/babyname.original.db > babyname.original.db` takes it out.

## Version control

The project is a Git repository. `include/Secrets.h` is excluded by `.gitignore`, as are PlatformIO's build folder, the tooling folder `.venv` and a saved `coredump.bin`. A fresh copy of the project needs a `Secrets.h` made from `Secrets.example.h` before it compiles.

Both database files are committed, since the project cannot be built into a working board without them.

## Reading a crash

There are two ways to find out where the firmware crashed. The second one, the saved crash dump, was tried on this board with a test firmware that wrote through a null pointer; the commands and the sample output below come from that test. The first is standard PlatformIO behaviour and was not tried here.

### Serial output

Only the Debug build writes to the serial port. `platformio.ini` passes `-DSERIAL_LOG=1` to it, and `include/Definitions.h` turns that into the macros `LOG_BEGIN`, `LOG_PRINT`, `LOG_PRINTLN` and `LOG_PRINTF`, which every message goes through. In the Release build the macros stand for nothing, so the serial port is not started and the messages are not even part of the firmware, which is about 26 KB smaller for it.

The Debug build prints:

- the startup steps: the WiFi connection or the fallback access point, the filesystem, the free PSRAM and the database cache, the name on the network, and the server starting;
- a waiting crash dump;
- one line per `/api/names` and `/api/count` request, saying how long the database work and the sending took, for example `Names 'Bob'..'(startswith)' ... database 12.54 ms, sending 11.11 ms (164 bytes in 1 chunks), whole response 30.4 ms`;
- the Arduino core's own messages at log level 5 (verbose).

Even the Release build is not completely silent: the ESP32's ROM prints a few lines when the chip starts, and a crash still prints its report (see below). Both come from below the firmware.

### While watching: the serial monitor

When the firmware crashes it prints the processor state, in either build, and a backtrace of addresses to the serial port, then restarts. Built and monitored with the Debug environment, the addresses are translated into function names and line numbers:

```
pio run -e Debug -t upload
pio device monitor -e Debug
```

This only helps if the monitor was running at the moment of the crash.

### Afterwards: the saved crash dump

The framework also saves the state of every task to the `coredump` partition before restarting. The dump stays there until it is erased or the next crash overwrites it, so it can be read later.

The board says when a dump is waiting. At startup the firmware checks the partition for a complete dump and reads from it which task crashed and at which address. Three places then show it:

- the serial port, as one line at startup (Debug build only);
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

- There is no login, by design (see [Purpose](#purpose)). Anyone on the network can query the database and read the device status, and anyone who can reach the setup page can change the network settings, which can take the board off the network. The board belongs on a trusted network only.
- The WiFi passwords are stored as plain text: the defaults in `include/Secrets.h`, and the saved one in NVS, which can be read out over the serial cable. `/api/settings` never sends the saved password back.
- The access point password can only be changed in `include/Secrets.h`.

**Reliability**

- The server answers one request at a time. A slow request delays everyone else. The largest single request is the first sorted page of a search that matches the whole table, about 3 s. The count for such a search, and a jump to its last page in name order, take about 0.7 s each.
- Serial uploads are unreliable on this board. Since 15 October 2026 it does not enter download mode by itself: the reset line from the USB adapter works, but the boot line (GPIO0) is not pulled low, with both versions of the upload tool. Uploading then needs the BOOT button held until "Connecting..." appears. On 16 October, with a second board unplugged from the same USB hub, uploads worked unattended again. Earlier in development it sometimes behaved the same way, and at other times stayed in download mode and did not start.

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

- Looking up the `.local` name takes about a second on Windows. The board's IP address is assigned by the router and changed during development (`.167`, `.185`, then `.167` again).
- A host name is changed by restarting the board. Until the router's and the PC's name caches catch up, the old name may still be found, and the new one not yet.
- While the access point runs, the captive DNS server gives the board's address for every name, so a device connected to it reaches nothing else by name. The access point never offered internet access, so nothing is lost, but a phone may switch back to mobile data on its own.
- Country and transmit power are applied as the ESP32 allows; the board does not check them against a country's legal power limit beyond the channel range.
- In access point mode the board leaves the WiFi network for good, until the mode is changed back on the setup page, through the access point at `192.168.4.1`. A WiFi network name or password typed wrongly has a similar effect: the board falls back to its access point after 20 seconds and tries again every five minutes.
- Other PlatformIO projects on the same PC can replace the shared framework and upload tool with versions this project does not build with. `platformio.ini` therefore gives this project its own packages folder (`packages_dir`), filled by the first build.

**Housekeeping**

- Reading a crash dump is a manual procedure. The board reports that a dump is waiting, but the dump can only be read and erased over the serial cable.

## Suggested improvements

In rough order of value.

1. **Give the board a fixed address.** Either fill in `STATIC_IP` and `STATIC_GATEWAY` in `include/Definitions.h` or reserve the address on the router. This avoids the name lookup and the changing address.
2. **Reconsider updates over WiFi.** They were added and removed during development because they need two firmware slots, which leaves about 1.1 MB for the filesystem. They would remove the dependence on the serial upload.

Not on this list, because the project is a read-only demonstration: writing to the database from the board, a login, and encryption. See [Purpose](#purpose).

Done since the first version of this document: version control, a content-based cache label for the page, no automatic formatting of the filesystem, an automated browser test, a row total, searching without a reload, column sorting (now of the whole search, on the board), first and last buttons, removal of the file listing, the Bootstrap stylesheet and pagination, the crash-dump notice, the compressed page, a one-command test run, serial output in the Debug build only, the access point after a lost connection, the `-O2` measurement, the crash-dump procedure, and the setup page with settings kept in NVS.
