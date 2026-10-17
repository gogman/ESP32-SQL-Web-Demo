/*
 * Every setting of the application that someone might want to change, in
 * one place. Nothing here is secret: the WiFi network name and all passwords
 * live in Secrets.h, which is kept out of version control.
 *
 * Most values are "static const" variables. A few are #define macros
 * instead; those are the strings that get glued to other string literals at
 * compile time (for example "/littlefs" DB_FILE), which only works with
 * macros.
 */

#ifndef DEFINITIONS_H
#define DEFINITIONS_H

/* ------------------------------------------------------------------ */
/* Network                                                            */
/* ------------------------------------------------------------------ */

/*
 * The board's default host name, used until another one is saved on the
 * setup page (/setup/). The network uses it in lower case, so the
 * application is reachable at http://esp32-sql.local. See include/settings.h.
 */
static const char DEFAULT_HOSTNAME[] = "ESP32-SQL";

/*
 * The start of the access point's default name. Six random hex digits are
 * added at the first startup, for example "ESP32 SQL 3FA9C1".
 */
static const char DEFAULT_AP_PREFIX[] = "ESP32 SQL";

/* The TCP port the web server listens on. 80 is the standard port for http. */
static const int HTTP_PORT = 80;

/*
 * How long connectWiFi() waits at startup for the WiFi network, in
 * milliseconds, before giving up and starting the fallback access point.
 */
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

/*
 * How long the WiFi connection may stay lost while the board is running,
 * in milliseconds, before the board gives up on it and starts the fallback
 * access point. Short drops are common and the framework reconnects by
 * itself within seconds, so only a lasting loss counts. 60000 ms is one
 * minute.
 */
static const unsigned long WIFI_LOST_TIMEOUT_MS = 60000;

/*
 * While the fallback access point is running and nobody is connected to it,
 * the board restarts this often (in milliseconds) to try the WiFi network
 * again. 300000 ms is five minutes.
 */
static const unsigned long AP_RETRY_MS = 300000;

/*
 * Optional fixed IP address.
 *
 * Leave STATIC_IP empty to let the network assign an address (DHCP); that
 * is the default. To use a fixed address, fill in STATIC_IP and
 * STATIC_GATEWAY (the router's address) and check STATIC_SUBNET. STATIC_DNS
 * may stay empty, in which case the gateway is used for name lookups.
 *
 * A fixed address lets you open the application by IP and skip the
 * "esp32-sql.local" name lookup, which can take about a second on Windows.
 * Make sure the address is outside the range your router hands out.
 */
static const char STATIC_IP[] = "";
static const char STATIC_GATEWAY[] = "";
static const char STATIC_SUBNET[] = "255.255.255.0";
static const char STATIC_DNS[] = "";

/* ------------------------------------------------------------------ */
/* Hardware                                                           */
/* ------------------------------------------------------------------ */

/* The GPIO pin of the LED that lights up while a request is being answered. */
static const int LED_PIN = 13;

/* ------------------------------------------------------------------ */
/* Storage                                                            */
/* ------------------------------------------------------------------ */

/*
 * The database file, as two different parts of the system name it:
 *
 * - DB_FILE is the path inside the LittleFS filesystem, used with the
 *   LittleFS object (for example to read the file's size and date).
 * - DB_PATH is the same file as SQLite opens it. SQLite uses the C file
 *   functions, which see the filesystem under its mount point "/littlefs".
 */
#define DB_FILE "/babyname.db"
static const char DB_PATH[] = "/littlefs" DB_FILE;

/*
 * The size of SQLite's page cache in KiB (1 KiB = 1024 bytes), used only
 * when the board has PSRAM. 1024 KiB is larger than the whole database
 * (about 816 KiB), so every page is read from flash at most once.
 */
static const int DB_CACHE_KIB = 1024;

/*
 * The most rows /api/names returns per request ("limit"). The page lets the
 * user choose up to this many. Larger pages mean fewer requests but a longer
 * wait for each one; 200 rows take roughly a tenth of a second.
 */
static const int DB_PAGE_ROWS = 200;

/*
 * The number of rows /api/names returns when the request does not say
 * ("limit"). The page starts at the same number, 25, the "selected" entry of
 * its "Rows per page" lists, so a change here must be made there as well.
 */
static const int DB_DEFAULT_ROWS = 25;

/* ------------------------------------------------------------------ */
/* Web addresses                                                      */
/* ------------------------------------------------------------------ */

/*
 * The addresses of the three JSON interfaces. The script in web/index.html
 * calls the same addresses, so a change here must be made there as well.
 */
#define NAMES_API_URL "/api/names"
#define COUNT_API_URL "/api/count"
#define STATUS_API_URL "/api/status"

/*
 * The setup page, where the network settings are changed, and the JSON
 * interface it reads and saves them through. web/setup.html calls the
 * same interface address.
 */
#define SETUP_PAGE_URL "/setup/"
#define SETTINGS_API_URL "/api/settings"

/*
 * How long the board waits after saving new settings before it restarts to
 * apply them, in milliseconds. The pause lets the answer to the save reach
 * the browser first.
 */
static const unsigned long RESTART_DELAY_MS = 1500;

/*
 * The Bootstrap stylesheet.
 *
 * CSS_URL is the address the page loads it from; web/index.html names the
 * same address. CSS_FILE is the gzip-compressed copy inside the filesystem
 * (data/bootstrap.min.css.gz in the project, made by tools/pack_css.py and
 * uploaded with "pio run -t uploadfs").
 */
#define CSS_URL "/bootstrap.min.css"
#define CSS_FILE "/bootstrap.min.css.gz"

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

/*
 * Output on the serial port, in the Debug build only.
 *
 * platformio.ini passes -DSERIAL_LOG=1 to the Debug build. The Release build
 * leaves it undefined, and this makes it 0: the serial port is then not even
 * started, and the firmware prints nothing of its own. (The ESP32's ROM
 * still prints a few boot lines, and a crash still prints its report; both
 * come from below the firmware.)
 *
 * All output goes through the LOG_ macros below, which mirror Serial.begin(),
 * print(), println() and printf(). In Release they are replaced by nothing,
 * so the text, and the work of formatting it, is left out of the firmware.
 * Their arguments are then not evaluated either, so they must not have side
 * effects the program relies on.
 *
 * In the Debug build this covers the startup messages, the network messages,
 * and one line per data request (/api/names, /api/count) saying how long the
 * database work and the sending took.
 */
#ifndef SERIAL_LOG
#define SERIAL_LOG 0
#endif

#if SERIAL_LOG
#define LOG_BEGIN(baud) Serial.begin(baud)
#define LOG_PRINT(...) Serial.print(__VA_ARGS__)
#define LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define LOG_BEGIN(baud) ((void)0)
#define LOG_PRINT(...) ((void)0)
#define LOG_PRINTLN(...) ((void)0)
#define LOG_PRINTF(...) ((void)0)
#endif

#endif
