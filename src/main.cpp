/*
 * SQLite_Web - a small web application on an ESP32 that searches a SQLite
 * database of baby names stored on the board's flash filesystem.
 *
 * This file is only the entry point. It creates the two long-lived objects
 * (the web server and the database manager), brings the board up in the
 * right order in setup(), and keeps everything running from loop().
 *
 * The real work lives in the other modules:
 *   network.cpp       - joining WiFi, the fallback access point, mDNS
 *   db_mgr.cpp        - opening the database and running the paged query
 *   web_handlers.cpp  - every address the web server answers
 *   chunk_server.h    - a WebServer that sends chunked responses efficiently
 *   Definitions.h     - all tunable settings in one place
 */

#include <Arduino.h>
#include "LittleFS.h"
#include "Definitions.h"
#include "chunk_server.h"
#include "db_mgr.h"
#include "network.h"
#include "web_handlers.h"

/*
 * The web server. It is the synchronous Arduino WebServer (one request at a
 * time, served from loop()), extended by ChunkServer so that streamed
 * responses cost one network packet per chunk instead of three.
 */
static ChunkServer server(HTTP_PORT);

/*
 * The database manager. One instance for the whole program: it owns the
 * SQLite connection, the prepared query and the buffer rows are written into.
 */
static db_mgr dbMgr;

/*
 * Runs once after power-on or reset.
 *
 * The order matters:
 *   1. LED and serial port first, so every later step can report progress.
 *   2. WiFi, which may take up to WIFI_CONNECT_TIMEOUT_MS and falls back to
 *      an access point if the network cannot be joined.
 *   3. The filesystem, because the database is a file on it. It is never
 *      formatted here; if it cannot be mounted, the board says so.
 *   4. The database, which needs the filesystem and (for its cache) PSRAM.
 *   5. mDNS and the web handlers, which need the network and the database.
 *   6. Finally the server starts listening.
 */
void setup(void)
{
  /* The LED shows request activity (see ActivityLed in web_handlers.cpp); start with it off. */
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, 0);

  /*
   * The serial port, in the Debug build only (see SERIAL_LOG in
   * Definitions.h). 115200 baud matches monitor_speed in platformio.ini. The
   * empty line separates our output from the boot messages the ESP32 ROM
   * prints first.
   */
  LOG_BEGIN(115200);
  LOG_PRINTLN("");

  /*
   * Join the WiFi network, or start the fallback access point. The return
   * value is not needed here: either way the board is reachable afterwards,
   * and handleNetwork() takes care of retrying the network later.
   */
  connectWiFi();

  /*
   * Mount the LittleFS filesystem that holds the database and the files
   * folder.
   *
   * The argument "false" means: do not format the partition if it cannot
   * be mounted. A failed mount almost always means that no filesystem image
   * has been uploaded to where the partition table says it is (a new board,
   * or a changed partition table). Formatting would hide that behind an
   * empty filesystem; leaving it alone keeps the cause visible and never
   * destroys data.
   *
   * A failure is not fatal. The web server still starts, so the problem is
   * reported on the page and by /api/status as well as here.
   */
  LOG_PRINTLN("Mounting LittleFS");
  const bool fsMounted = LittleFS.begin(false);

  if (fsMounted)
    LOG_PRINTLN("LittleFS mounted");
  else
    LOG_PRINTLN("LittleFS could not be mounted. Upload the filesystem image: pio run -e Release -t uploadfs");

  /*
   * Decide how much memory SQLite may use to cache database pages.
   *
   * With PSRAM (the 4 MB of external RAM on WROVER modules) the cache is
   * made large enough to hold the entire database, so each page is read
   * from flash only once. Without PSRAM the internal heap is far too small
   * for that, so 0 is passed and SQLite keeps its tiny built-in default.
   */
  const int cacheKiB = psramFound() ? DB_CACHE_KIB : 0;
  LOG_PRINTF("PSRAM: %u bytes free, database cache: %d KiB\n", (unsigned)ESP.getFreePsram(), cacheKiB);

  /*
   * Open the database. A failure here is reported but is not fatal: the web
   * server still starts, the page still loads, and every query answers with
   * the error text, which is far easier to diagnose than a board that
   * appears dead. Without a filesystem the attempt is still made, so that
   * the database object holds an error message to show.
   */
  if (!dbMgr.open(DB_PATH, cacheKiB))
  {
    LOG_PRINT("Failed to open database: ");
    LOG_PRINTLN(dbMgr.lastError());
  }

  /* Make the board reachable as "<MDNS_HOSTNAME>.local" instead of only by IP address. */
  announceWebServer();

  /*
   * Tell the server which function answers which address, then start
   * listening. cacheKiB and fsMounted are passed along so the status can
   * report them.
   */
  registerWebHandlers(server, dbMgr, cacheKiB, fsMounted);
  server.begin();

  LOG_PRINTLN("HTTP server started");
}

/*
 * Runs over and over for as long as the board is powered.
 */
void loop(void)
{
  /*
   * Accept a waiting browser connection, if there is one, and run the
   * matching handler to completion. Requests are therefore served strictly
   * one after another.
   */
  server.handleClient();

  /* Housekeeping for the network: retries the WiFi network after a fallback. */
  handleNetwork();

  /*
   * Sleep for one millisecond. Without this the loop would spin as fast as
   * it can and keep a CPU core at 100% while doing nothing. The delay lets
   * the idle task run (saving power and feeding the watchdog) at the cost
   * of at most one millisecond of extra latency per request.
   */
  delay(1);
}
