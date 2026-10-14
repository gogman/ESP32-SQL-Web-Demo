/*
 * Every address the web server answers.
 *
 * registerWebHandlers() at the bottom of this file is the only function
 * visible from outside; it connects each address to one of the handler
 * functions above it. A handler takes no arguments and returns nothing: it
 * reads the request from the server object and sends the response through
 * it.
 *
 * All addresses accept GET requests only.
 */

#include "web_handlers.h"
#include <WiFi.h>
#include "LittleFS.h"
#include "esp_core_dump.h"
#include "Definitions.h"

/*
 * The database hands its output over in pieces of up to db_mgr::CHUNK_SIZE
 * bytes, and each piece is sent with ChunkServer::sendChunk(), which packs
 * at most MAX_CHUNK_SIZE bytes into one packet. If the first number ever
 * grew beyond the second, sending would silently fall back to the slow
 * path. This check turns that into a compile error instead.
 */
static_assert(db_mgr::CHUNK_SIZE <= ChunkServer::MAX_CHUNK_SIZE, "a database chunk must fit in one write");

/*
 * The application page, web/index.html, gzip-compressed.
 *
 * It is not read from the filesystem. tools/pack_page.py compresses it into
 * web/index.html.gz before every build, and board_build.embed_files in
 * platformio.ini makes the build copy that file into the firmware itself.
 * The linker then provides two symbols marking where that copy begins and
 * ends. Their names are generated from the file's path, with every
 * character that is not a letter or digit replaced by an underscore. The
 * asm("...") part ties our readable C++ names to those generated ones.
 */
extern const char INDEX_HTML_START[] asm("_binary_web_index_html_gz_start");
extern const char INDEX_HTML_END[] asm("_binary_web_index_html_gz_end");

/* The page's icon, web/favicon.ico, built into the firmware the same way. */
extern const char FAVICON_START[] asm("_binary_web_favicon_ico_start");
extern const char FAVICON_END[] asm("_binary_web_favicon_ico_end");

/*
 * When this file was compiled: the date and time the compiler ran. It is
 * reported by /api/status as "build", to tell which firmware is on the board.
 *
 * It changes only when this particular file is recompiled, so it is a good
 * hint but not an exact fingerprint of the whole firmware.
 */
static const char BUILD_TIME[] = __DATE__ " " __TIME__;

/*
 * The "ETag" of the application page: a version label defined by HTTP.
 *
 * The browser remembers the page together with this label and, on the next
 * visit, asks "has it changed from this label?". For that to work the label
 * must change exactly when the page does. It is therefore worked out from
 * the page itself, in registerWebHandlers(): the page's length and a
 * checksum of its content, in double quotes as the ETag format requires,
 * for example "10b8-5f3a9c21".
 */
static char pageEtag[24] = "";

/*
 * The largest character that exists, U+10FFFF, encoded in UTF-8.
 *
 * A "names starting with X" search is run as the range from "X" to "X"
 * followed by this character. Every name that begins with X sorts between
 * those two, because whatever follows the X is smaller than the largest
 * possible character.
 */
static const char HIGHEST_CHARACTER[] = "\xF4\x8F\xBF\xBF";

/*
 * What the handlers work with. The web server calls a handler as a plain
 * function with no arguments, so there is no way to pass these in; they are
 * stored here by registerWebHandlers() instead.
 */
static ChunkServer *server;
static db_mgr *database;
static int databaseCacheKiB;
static bool filesystemMounted;

/*
 * A short text that identifies the database file currently on the board:
 * its size and its modification time, for example "835584-1791902046".
 *
 * It is worked out once at startup and reported by /api/status. The page
 * script adds it to every /api/names request, which is what allows those
 * responses to be cached by the browser safely: when a new database is
 * uploaded, the text changes, the addresses change with it, and the old
 * cached answers are never used again.
 */
static char databaseVersion[32] = "";

/*
 * Whether a crash dump is waiting in the coredump partition, and if so,
 * which task crashed and at which address in the program.
 *
 * When the firmware crashes, the framework saves the state of every task to
 * that partition before restarting. The dump stays there until it is erased
 * or the next crash overwrites it, so "true" means "there was a crash at
 * some point, and it can still be examined", not "the board just crashed".
 * It is looked up once at startup and reported by /api/status.
 */
static bool crashDumpWaiting;
static char crashTask[16] = "";
static uint32_t crashAddress;

/*
 * Switches the LED on for as long as a request is being answered.
 *
 * A handler creates one of these as its first statement. The constructor
 * turns the LED on; the destructor turns it off when the handler returns,
 * including through any of its early "return" statements.
 */
struct ActivityLed
{
  ActivityLed() { digitalWrite(LED_PIN, 1); }
  ~ActivityLed() { digitalWrite(LED_PIN, 0); }
};

/*
 * Copies text into a buffer in a form that can be placed between the double
 * quotes of a JSON string.
 *
 * The texts treated this way are error messages, which may come from SQLite
 * and contain anything. A double quote, a backslash or a control character
 * would break the JSON, so each is replaced by an apostrophe. That loses a
 * little fidelity but keeps the message readable, which is all an error
 * message needs. Text longer than the buffer is cut short.
 */
static void copyJsonSafe(char *out, size_t size, const char *text)
{
  strlcpy(out, text, size);

  for (char *c = out; *c; c++)
  {
    if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20)
      *c = '\'';
  }
}

/*
 * Sends an error as a small JSON document, {"error":"<message>"}, with the
 * given HTTP status code (400 for a bad request, 500 for a server failure).
 */
static void sendJsonError(int code, const char *message)
{
  char safe[120];
  copyJsonSafe(safe, sizeof(safe), message);

  char body[160];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", safe);

  /* An error describes this moment only; the browser must not store and reuse it. */
  server->sendHeader("Cache-Control", "no-store");
  server->send(code, "application/json", body);
}

/*
 * GET / and GET /query_db - the application page.
 *
 * The page is static, so the aim is to send it as rarely as possible.
 * "Cache-Control: no-cache" does not forbid caching; it tells the browser
 * to keep its copy but to check with the server before each use. The check
 * is the If-None-Match header carrying the ETag the browser was given. If
 * that still equals the label of the page in this firmware, the answer is a
 * bodiless "304 Not Modified" and the browser uses its copy.
 */
static void handleIndex()
{
  ActivityLed led;

  /* Both headers go out with either answer, the full page and the 304. */
  server->sendHeader("ETag", pageEtag);
  server->sendHeader("Cache-Control", "no-cache");

  if (server->header("If-None-Match") == pageEtag)
  {
    server->send(304, "text/html", "");
    return;
  }

  /*
   * Send the embedded page straight from flash, without copying it. It is
   * stored gzip-compressed (see tools/pack_page.py) and sent exactly as
   * stored; "Content-Encoding: gzip" tells the browser to unpack it. Every
   * browser accepts gzip, so no uncompressed copy is kept. The length is
   * the distance between the two linker symbols, the same length the ETag
   * was computed over.
   */
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", INDEX_HTML_START, INDEX_HTML_END - INDEX_HTML_START);
}

/*
 * Reads the rows a request asks for: the range of names into from and to,
 * and the primary sex filter into sex.
 *
 * The range is given either as "prefix" (names starting with that text) or
 * as "from" and "to" together. The filter is the optional parameter "sex":
 * "M" or "F" (either case), or left out or empty for all rows.
 *
 * Returns true when the request is valid. Otherwise it is answered with a
 * 400 error here, false is returned, and the caller has nothing left to do.
 */
static bool readRange(String &from, String &to, String &sex)
{
  sex = server->arg("sex");
  sex.toUpperCase();

  if (sex != "" && sex != "M" && sex != "F")
  {
    sendJsonError(400, "sex must be M, F or empty");
    return false;
  }

  if (server->hasArg("prefix"))
  {
    /* "Starts with" becomes a range; see HIGHEST_CHARACTER. */
    from = server->arg("prefix");
    to = from + HIGHEST_CHARACTER;
    return true;
  }

  if (server->hasArg("from") && server->hasArg("to"))
  {
    from = server->arg("from");
    to = server->arg("to");
    return true;
  }

  sendJsonError(400, "Give either prefix, or from and to");
  return false;
}

/*
 * The Cache-Control header for an answer that depends only on the database.
 *
 * A request that carries the database version (the parameter "v") always
 * gives the same answer for the same address, because a different database
 * would have a different version and so a different address. Such an answer
 * may be kept by the browser for a day (86400 seconds); "private" restricts
 * that to the user's own browser. Without the version there is no such
 * guarantee, and the browser has to ask again each time.
 */
static const char *dataCacheControl()
{
  return server->hasArg("v") ? "private, max-age=86400" : "no-cache";
}

/*
 * GET /api/names - one page of names, as JSON.
 *
 * Parameters (in the query string):
 *
 *   prefix               Names starting with this text. Or instead of it:
 *   from, to             The first and last name of a range.
 *   after_name,
 *   after_year,
 *   after_state          Optional. The last row of the previous page; this
 *                        page continues after it. Left out for the first page.
 *   offset               Optional, and only used when no cursor is given. The
 *                        number of rows of the range before this page. It
 *                        reaches any page directly, at a cost that grows with
 *                        the number; the page uses it for jumps such as "last".
 *   sort                 Optional. The column to sort the whole range by, as
 *                        its position in a row, 0 to 6. Pages of a sorted
 *                        range are found by offset only; a cursor is ignored.
 *   desc                 Optional, with sort. "1" sorts largest first.
 *   sex                  Optional. "M" or "F" keeps only rows with that
 *                        primary sex; left out or empty, all rows.
 *   limit                Optional. Rows per page, 1 to DB_PAGE_ROWS;
 *                        DB_DEFAULT_ROWS when left out.
 *   v                    Optional. The database version from /api/status.
 *                        Its value is not examined; its presence marks the
 *                        response as safe to cache.
 *
 * The answer is the document described at db_mgr::getPage(), with status
 * 200. A request without a range, or with a sort column, filter or limit
 * that does not exist, gets 400, a database failure 500, both with a {"error": ...}
 * document.
 */
static void handleNamesApi()
{
  ActivityLed led;

  const unsigned long responseStart = micros();

  /*
   * The range to search. These are String objects declared here, rather
   * than temporaries, because the database uses the character data in
   * place (see db_mgr::getPage): it has to stay alive until the query is
   * finished.
   */
  String from;
  String to;
  String sex;

  if (!readRange(from, to, sex))
    return;

  /*
   * Rows per page. toInt() gives 0 for anything that is not a number,
   * which the range check then refuses.
   */
  const int limit = server->hasArg("limit") ? server->arg("limit").toInt() : DB_DEFAULT_ROWS;

  if (limit < 1 || limit > DB_PAGE_ROWS)
  {
    char message[48];
    snprintf(message, sizeof(message), "limit must be from 1 to %d", DB_PAGE_ROWS);
    sendJsonError(400, message);
    return;
  }

  /*
   * The cursor. The strings are again held in named objects so they outlive
   * the query. Whether a cursor was given at all is decided by the presence
   * of after_name; without it, the first page is asked for.
   */
  const String afterName = server->arg("after_name");
  const String afterState = server->arg("after_state");
  const db_mgr::Cursor after = {afterName.c_str(), server->arg("after_year").toDouble(), afterState.c_str()};
  /*
   * The sort order, if one was asked for. The column must be a single digit
   * within range; anything else is refused rather than guessed at.
   */
  const bool hasSort = server->hasArg("sort");
  const String sortArg = server->arg("sort");
  const int sortColumn = sortArg.length() == 1 ? sortArg[0] - '0' : -1;
  const bool descending = server->arg("desc") == "1";

  if (hasSort && (sortColumn < 0 || sortColumn >= db_mgr::COLUMN_COUNT))
  {
    sendJsonError(400, "sort must be a column number from 0 to 6");
    return;
  }

  /* A cursor only means something in the default order, so sorting switches it off. */
  const bool hasCursor = !hasSort && server->hasArg("after_name");

  /*
   * The alternative to a cursor: the position of the page in the range.
   * A cursor wins when both are given, because it is the cheaper way.
   */
  const bool hasOffset = !hasCursor && server->hasArg("offset");
  const long offset = server->arg("offset").toInt();

  /*
   * Measurements for the timing log. The time spent sending is added up
   * separately, so the log can show database work and network work apart.
   */
  unsigned long sendMicros = 0;
  size_t bytes = 0;
  unsigned int chunks = 0;

  const unsigned long pageStart = micros();

  /*
   * The function that receives the output of the query, written as a
   * lambda: an unnamed function defined on the spot. The "[&]" lets it use
   * the local variables of this handler directly. It is called once for
   * every buffer-full of JSON the database produces.
   */
  const db_mgr::ChunkSink sendPiece = [&](const char *data, size_t length)
                                  {
                                    const unsigned long sendStart = micros();

                                    /*
                                     * The HTTP status line and headers are sent here, with the first
                                     * piece of data, and not before the query starts. If the query
                                     * fails before producing anything, nothing has been sent yet and
                                     * the handler can still answer with a 500 status below.
                                     *
                                     * CONTENT_LENGTH_UNKNOWN switches the response to chunked
                                     * encoding: the total size is not known in advance, and the
                                     * document is sent piece by piece without ever being held in
                                     * memory as a whole. The empty body in send() means "headers only".
                                     */
                                    if (chunks == 0)
                                    {
                                      server->sendHeader("Cache-Control", dataCacheControl());
                                      server->setContentLength(CONTENT_LENGTH_UNKNOWN);
                                      server->send(200, "application/json", "");
                                    }

                                    server->sendChunk(data, length);
                                    sendMicros += micros() - sendStart;
                                    bytes += length;
                                    chunks++; };

  /* Sorted if asked for; else by position if one was given; otherwise by cursor (or the first page). */
  const bool result = hasSort
                          ? database->getSortedPage(from.c_str(), to.c_str(), sex.c_str(), sortColumn, descending, offset, limit, sendPiece)
                      : hasOffset
                          ? database->getPageAt(from.c_str(), to.c_str(), sex.c_str(), offset, limit, sendPiece)
                          : database->getPage(from.c_str(), to.c_str(), sex.c_str(), hasCursor ? &after : nullptr, limit, sendPiece);

  const unsigned long pageMicros = micros() - pageStart;

  /*
   * If anything was sent, the response is under way and has to be ended
   * properly. If nothing was sent, the query failed before its first
   * output, and the failure becomes a regular error response.
   */
  if (chunks > 0)
    server->endChunks();
  else
    sendJsonError(500, database->lastError());

  if (SERIAL_LOG)
  {
    /*
     * One line per request on the serial port. pageMicros covers the whole
     * getPage() call, which includes the time spent inside the lambda, so
     * the sending time is subtracted to leave the database's share: running
     * the query and writing the rows as JSON.
     */
    LOG_PRINTF("Names '%s'..'%s' after '%s' offset %ld sort %d%s %s: database %.2f ms, sending %.2f ms (%u bytes in %u chunks), whole response %.2f ms\n",
                  from.c_str(), server->hasArg("prefix") ? "(prefix)" : to.c_str(), afterName.c_str(),
                  hasOffset ? offset : 0L, hasSort ? sortColumn : -1, descending ? " desc" : "", result ? "ok" : database->lastError(),
                  (pageMicros - sendMicros) / 1000.0, sendMicros / 1000.0, (unsigned)bytes, chunks,
                  (micros() - responseStart) / 1000.0);
  }
}

/*
 * GET /api/count - how many names a range holds, as JSON: {"count":1234}.
 *
 * It takes the same prefix or from/to parameters as /api/names, and "v" for
 * caching. The page asks for it once per search, to show "Rows 1 to 200 of
 * 1234". It is a separate address, rather than part of every page of rows,
 * because counting reads the whole range: doing it once per search is cheap,
 * doing it for every page would not be.
 */
static void handleCountApi()
{
  ActivityLed led;

  String from;
  String to;
  String sex;

  if (!readRange(from, to, sex))
    return;

  const unsigned long countStart = micros();
  const long count = database->getCount(from.c_str(), to.c_str(), sex.c_str());

  if (count < 0)
  {
    sendJsonError(500, database->lastError());
    return;
  }

  char body[32];
  snprintf(body, sizeof(body), "{\"count\":%ld}", count);

  server->sendHeader("Cache-Control", dataCacheControl());
  server->send(200, "application/json", body);

  if (SERIAL_LOG)
  {
    LOG_PRINTF("Count '%s'..'%s': %ld rows, whole response %.2f ms\n",
                  from.c_str(), server->hasArg("prefix") ? "(prefix)" : to.c_str(), count,
                  (micros() - countStart) / 1000.0);
  }
}

/*
 * GET /api/status - the state of the device, as JSON.
 *
 * The page uses several of the values: uptime (seconds since start),
 * db_version, fs_mounted, db_open and db_error to warn when the data is
 * not available, and crash_dump, crash_task and crash_pc to say that a
 * crash was recorded. The rest is for diagnosis: which firmware build is
 * running, how the board is connected, and how much memory is free.
 */
static void handleStatusApi()
{
  ActivityLed led;

  /* In access point mode the board's own address is the access point's, not a station address. */
  const bool accessPoint = WiFi.getMode() == WIFI_AP;
  const String ip = (accessPoint ? WiFi.softAPIP() : WiFi.localIP()).toString();

  /*
   * Why the database is not open, if it is not. While it is open the text
   * is left empty: lastError() keeps the message of some earlier failed
   * query, which would be misleading here.
   */
  char databaseError[100] = "";
  if (!database->isOpen())
    copyJsonSafe(databaseError, sizeof(databaseError), database->lastError());

  /*
   * The document is small and of predictable size, so it is built in one
   * buffer and sent in one piece.
   */
  char body[600];
  snprintf(body, sizeof(body),
           "{\"uptime\":%lu,\"build\":\"%s\",\"network\":\"%s\",\"ip\":\"%s\","
           "\"fs_mounted\":%s,\"db_open\":%s,\"db_error\":\"%s\",\"db_version\":\"%s\",\"db_cache_kib\":%d,"
           "\"crash_dump\":%s,\"crash_task\":\"%s\",\"crash_pc\":\"0x%08x\","
           "\"heap_free\":%u,\"psram_size\":%u,\"psram_free\":%u,\"flash_size\":%u}",
           millis() / 1000, BUILD_TIME, accessPoint ? "access point" : "station", ip.c_str(),
           filesystemMounted ? "true" : "false", database->isOpen() ? "true" : "false", databaseError,
           databaseVersion, databaseCacheKiB,
           crashDumpWaiting ? "true" : "false", crashTask, (unsigned)crashAddress,
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
           (unsigned)ESP.getFlashChipSize());

  /* The status changes every second, so it must never come from a cache. */
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", body);
}

/*
 * GET /bootstrap.min.css - the Bootstrap stylesheet.
 *
 * The file on the filesystem is gzip-compressed (about 31 KB instead of
 * 232 KB) and is sent exactly as stored; the browser unpacks it. Every
 * browser accepts gzip, so no uncompressed copy is kept.
 */
static void handleCss()
{
  ActivityLed led;

  /* Without a mounted filesystem there is no stylesheet; the page then shows unstyled. */
  File file = filesystemMounted && LittleFS.exists(CSS_FILE) ? LittleFS.open(CSS_FILE) : File();

  if (!file)
  {
    server->sendHeader("Cache-Control", "no-store");
    server->send(404, "text/plain", "The stylesheet is not on the filesystem\n");
    return;
  }

  /*
   * The page asks for the stylesheet with its version in the address
   * (?v=5.3.8-1), so a different version has a different address and this
   * answer can be kept by the browser for a year without asking again.
   *
   * streamFile() sends the size and then the content in blocks. Because
   * the file's name ends in ".gz", it also adds "Content-Encoding: gzip",
   * which is what tells the browser to unpack it.
   */
  server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server->streamFile(file, "text/css");
}

/*
 * GET /favicon.ico - the icon the browser shows for the page, in its tab
 * and its bookmarks: the Espressif logo, at 16, 32 and 48 pixels.
 *
 * Browsers ask for it by themselves, on the first visit and again from
 * time to time. It never changes between firmware versions in practice,
 * so the browser may keep it for a week without asking.
 */
static void handleFavicon()
{
  ActivityLed led;

  server->sendHeader("Cache-Control", "public, max-age=604800");
  server->send_P(200, "image/x-icon", FAVICON_START, FAVICON_END - FAVICON_START);
}

/*
 * Called for every request that no registered address matches.
 *
 * That covers two cases:
 *
 * 1. A method other than GET. Every address is registered for GET only, so
 *    a POST to an existing address also lands here. It is answered with
 *    "405 Method Not Allowed" and an Allow header naming what is accepted.
 * 2. Everything else, which is a plain "404 Not Found".
 */
static void handleOther()
{
  ActivityLed led;

  if (server->method() != HTTP_GET)
  {
    server->sendHeader("Allow", "GET");
    server->send(405, "text/plain", "Method Not Allowed\n");
    return;
  }

  const String uri = server->uri();

  /*
   * Build the 404 text. reserve() sizes the String once for the fixed words
   * plus the address, instead of letting it grow in steps as parts are added.
   */
  String message;
  message.reserve(64 + uri.length());
  message = "File Not Found\n\nURI: ";
  message += uri;
  message += "\n";

  server->send(404, "text/plain", message);
}

/*
 * Connects the addresses to the handlers; see web_handlers.h for the list.
 */
void registerWebHandlers(ChunkServer &webServer, db_mgr &db, int cacheKiB, bool fsMounted)
{
  /* Keep what the handlers need (see the note at the variables above). */
  server = &webServer;
  database = &db;
  databaseCacheKiB = cacheKiB;
  filesystemMounted = fsMounted;

  /*
   * Work out the page's ETag from its content. The checksum is FNV-1a, a
   * simple and well-known hash: start from a fixed value, and for every
   * byte, combine it in with exclusive-or and multiply by a fixed prime.
   * Any change to the page changes the result. The page is a few kilobytes,
   * so this takes a fraction of a millisecond, once, at startup.
   */
  const size_t pageLength = INDEX_HTML_END - INDEX_HTML_START;
  uint32_t checksum = 2166136261u;

  for (size_t i = 0; i < pageLength; i++)
  {
    checksum ^= (uint8_t)INDEX_HTML_START[i];
    checksum *= 16777619u;
  }

  snprintf(pageEtag, sizeof(pageEtag), "\"%x-%08x\"", (unsigned)pageLength, (unsigned)checksum);

  /*
   * Work out the database version text from the file's size and its last
   * modification time. If the file is missing, or there is no filesystem
   * to look in, the text stays empty; the page then sends no version and
   * its requests are simply not cached.
   */
  File file = fsMounted ? LittleFS.open(DB_FILE) : File();
  if (file)
    snprintf(databaseVersion, sizeof(databaseVersion), "%u-%lu", (unsigned)file.size(), (unsigned long)file.getLastWrite());

  /*
   * Look for a crash dump. esp_core_dump_image_check() verifies that the
   * coredump partition holds a complete dump with a correct checksum; an
   * erased partition, or none at all, makes it report an error. The summary
   * then gives the name of the task that crashed and the program address it
   * crashed at. The task name is made safe for JSON, since it is read from
   * flash and reported inside a JSON string.
   */
  crashDumpWaiting = esp_core_dump_image_check() == ESP_OK;

  if (crashDumpWaiting)
  {
    /* static: the summary is a few hundred bytes, too much to put on the stack for one use. */
    static esp_core_dump_summary_t summary;

    if (esp_core_dump_get_summary(&summary) == ESP_OK)
    {
      copyJsonSafe(crashTask, sizeof(crashTask), summary.exc_task);
      crashAddress = summary.exc_pc;
    }

    LOG_PRINTF("A crash dump is waiting: task '%s' at 0x%08x. See docs/how-it-works.md, \"Reading a crash\"\n",
                  crashTask, (unsigned)crashAddress);
  }

  /*
   * The web server only keeps the request headers it has been told to
   * collect; all others are discarded while the request is read. Without
   * this, server->header("If-None-Match") in handleIndex() would always be
   * empty and the page would be sent in full every time.
   */
  static const char *HEADERS[] = {"If-None-Match"};
  webServer.collectHeaders(HEADERS, 1);

  /*
   * The page. /query_db was the address of the separate results page in an
   * earlier version; it now shows the same page, so bookmarks and links to
   * it keep working.
   */
  webServer.on("/", HTTP_GET, handleIndex);
  webServer.on("/query_db", HTTP_GET, handleIndex);

  /* The three JSON interfaces the page script calls. */
  webServer.on(NAMES_API_URL, HTTP_GET, handleNamesApi);
  webServer.on(COUNT_API_URL, HTTP_GET, handleCountApi);
  webServer.on(STATUS_API_URL, HTTP_GET, handleStatusApi);

  /* The stylesheet the page links to, and the page's icon. */
  webServer.on(CSS_URL, HTTP_GET, handleCss);
  webServer.on("/favicon.ico", HTTP_GET, handleFavicon);

  /* All errors. */
  webServer.onNotFound(handleOther);
}
