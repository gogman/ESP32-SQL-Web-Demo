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
 * The name the board announces on the local network. With "esp32" the
 * application is reachable at http://esp32.local.
 */
static const char MDNS_HOSTNAME[] = "esp32";

/* The TCP port the web server listens on. 80 is the standard port for http. */
static const int HTTP_PORT = 80;

/*
 * How long connectWiFi() waits at startup for the WiFi network, in
 * milliseconds, before giving up and starting the fallback access point.
 */
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

/*
 * The network name of the fallback access point. Its password is
 * AP_PASSWORD in Secrets.h. When connected to it, the application is at
 * http://192.168.4.1.
 */
static const char AP_SSID[] = "esp32-sqlite";

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
 * "esp32.local" name lookup, which can take about a second on Windows.
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
 * How many rows /api/names returns per request, and therefore how many rows
 * the page shows at a time. Larger pages mean fewer requests but a longer
 * wait for each one; 200 rows take roughly a tenth of a second.
 */
static const int DB_PAGE_ROWS = 200;

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
 * Downloadable files.
 *
 * FILES_DIR is the folder inside the filesystem (data/files in the project,
 * uploaded with "pio run -t uploadfs"). FILES_URL is the address it is
 * published under. The _SLASH variants are the same with a trailing slash,
 * needed where a path is built by appending a file name.
 */
#define FILES_DIR "/files"
#define FILES_DIR_SLASH FILES_DIR "/"
#define FILES_URL "/files"
#define FILES_URL_SLASH FILES_URL "/"

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

/*
 * When true, every /api/names request prints one line to the serial port
 * showing how long the database work and the sending took. Set it to false
 * to keep the serial output quiet.
 */
static const bool LOG_QUERY_TIMING = true;

#endif
