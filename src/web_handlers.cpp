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
 * The application page, web/index.html.
 *
 * It is not read from the filesystem: board_build.embed_txtfiles in
 * platformio.ini makes the build copy the file into the firmware itself.
 * The linker then provides two symbols marking where that copy begins and
 * ends. Their names are generated from the file's path, with every
 * character that is not a letter or digit replaced by an underscore. The
 * asm("...") part ties our readable C++ names to those generated ones.
 */
extern const char INDEX_HTML_START[] asm("_binary_web_index_html_start");
extern const char INDEX_HTML_END[] asm("_binary_web_index_html_end");

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
 * The look of the files page, matching the application page. It is a macro
 * rather than a variable so it can be joined to the string literal below at
 * compile time.
 */
#define PAGE_STYLE "<style>body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; font-size: large; Color: #000088; }</style>"

/*
 * The fixed pieces of the files page. The page is sent as: head, then
 * either the table (head, one row per file, end) or the "no files" line,
 * then the end piece with the back button.
 */
static const char FILES_PAGE_HEAD[] =
    "<html><head><title>ESP32 Files</title>" PAGE_STYLE "</head><body><h1>Files</h1>";

static const char FILES_TABLE_HEAD[] = "<table cellspacing='1' cellpadding='1' border='1'><tr><td>Name</td><td>Size (bytes)</td></tr>";

static const char FILES_NO_FILES[] = "<p>No files found</p>";

static const char FILES_TABLE_END[] = "</table>";

static const char FILES_PAGE_END[] = "<p><input type=button onclick='location.href=\"/\"' value='back'/></p></body></html>";

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
 * Returns a copy of text that is safe to place inside HTML.
 *
 * The five characters that have a meaning in HTML are replaced by their
 * named or numbered entities. Without this, a file called "<b>x" would be
 * read by the browser as markup rather than shown as a name. Both kinds of
 * quote are included because the result is also used inside an attribute.
 */
static String htmlEscaped(const char *text)
{
  String escaped;

  for (; *text; text++)
  {
    switch (*text)
    {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    case '\'':
      escaped += "&#39;";
      break;
    default:
      escaped += *text;
    }
  }

  return escaped;
}

/*
 * Works out the Content-Type header for a file from the end of its name.
 *
 * The browser uses this to decide what to do with a download: show it as
 * text, display it as an image, and so on. Anything not in the table is
 * sent as "application/octet-stream", meaning "arbitrary data", which makes
 * the browser offer to save the file.
 */
static const char *contentType(const String &path)
{
  static const struct
  {
    const char *extension;
    const char *type;
  } TYPES[] = {
      {".html", "text/html"},
      {".htm", "text/html"},
      {".css", "text/css"},
      {".js", "application/javascript"},
      {".json", "application/json"},
      {".txt", "text/plain"},
      {".me", "text/plain"},
      {".csv", "text/csv"},
      {".png", "image/png"},
      {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"},
      {".gif", "image/gif"},
      {".svg", "image/svg+xml"},
      {".ico", "image/x-icon"},
      {".pdf", "application/pdf"},
  };

  for (const auto &entry : TYPES)
  {
    if (path.endsWith(entry.extension))
      return entry.type;
  }

  return "application/octet-stream";
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
   * Send the embedded page straight from flash, without copying it. The
   * length is the distance between the two linker symbols, minus one
   * because the build adds a terminating zero after the file's content
   * (the same length the ETag was computed over).
   */
  server->send_P(200, "text/html", INDEX_HTML_START, INDEX_HTML_END - INDEX_HTML_START - 1);
}

/*
 * Reads the range of names a request asks for into from and to.
 *
 * The range is given either as "prefix" (names starting with that text) or
 * as "from" and "to" together. Returns true when one of the two forms was
 * present. Otherwise the request is answered with a 400 error here, false
 * is returned, and the caller has nothing left to do.
 */
static bool readRange(String &from, String &to)
{
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
 *   v                    Optional. The database version from /api/status.
 *                        Its value is not examined; its presence marks the
 *                        response as safe to cache.
 *
 * The answer is the document described at db_mgr::getPage(), with status
 * 200. A request without a range gets 400, a database failure 500, both
 * with a {"error": ...} document.
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

  if (!readRange(from, to))
    return;

  /*
   * The cursor. The strings are again held in named objects so they outlive
   * the query. Whether a cursor was given at all is decided by the presence
   * of after_name; without it, the first page is asked for.
   */
  const String afterName = server->arg("after_name");
  const String afterState = server->arg("after_state");
  const db_mgr::Cursor after = {afterName.c_str(), server->arg("after_year").toDouble(), afterState.c_str()};
  const bool hasCursor = server->hasArg("after_name");

  /*
   * Measurements for the timing log. The time spent sending is added up
   * separately, so the log can show database work and network work apart.
   */
  unsigned long sendMicros = 0;
  size_t bytes = 0;
  unsigned int chunks = 0;

  const unsigned long pageStart = micros();

  /*
   * Run the query. The last argument is the function that receives the
   * output, written as a lambda: an unnamed function defined on the spot.
   * The "[&]" lets it use the local variables of this handler directly. It
   * is called once for every buffer-full of JSON the database produces.
   */
  bool result = database->getPage(from.c_str(), to.c_str(), hasCursor ? &after : nullptr, DB_PAGE_ROWS,
                                  [&](const char *data, size_t length)
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
                                    chunks++; });

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

  if (LOG_QUERY_TIMING)
  {
    /*
     * One line per request on the serial port. pageMicros covers the whole
     * getPage() call, which includes the time spent inside the lambda, so
     * the sending time is subtracted to leave the database's share: running
     * the query and writing the rows as JSON.
     */
    Serial.printf("Names '%s'..'%s' after '%s' %s: database %.2f ms, sending %.2f ms (%u bytes in %u chunks), whole response %.2f ms\n",
                  from.c_str(), server->hasArg("prefix") ? "(prefix)" : to.c_str(), afterName.c_str(),
                  result ? "ok" : database->lastError(),
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

  if (!readRange(from, to))
    return;

  const unsigned long countStart = micros();
  const long count = database->getCount(from.c_str(), to.c_str());

  if (count < 0)
  {
    sendJsonError(500, database->lastError());
    return;
  }

  char body[32];
  snprintf(body, sizeof(body), "{\"count\":%ld}", count);

  server->sendHeader("Cache-Control", dataCacheControl());
  server->send(200, "application/json", body);

  if (LOG_QUERY_TIMING)
  {
    Serial.printf("Count '%s'..'%s': %ld rows, whole response %.2f ms\n",
                  from.c_str(), server->hasArg("prefix") ? "(prefix)" : to.c_str(), count,
                  (micros() - countStart) / 1000.0);
  }
}

/*
 * GET /api/status - the state of the device, as JSON.
 *
 * The page uses several of the values: uptime (seconds since start),
 * db_version, and fs_mounted, db_open and db_error to warn when the data is
 * not available. The rest is for diagnosis: which firmware build is running,
 * how the board is connected, and how much memory is free.
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
  char body[512];
  snprintf(body, sizeof(body),
           "{\"uptime\":%lu,\"build\":\"%s\",\"network\":\"%s\",\"ip\":\"%s\","
           "\"fs_mounted\":%s,\"db_open\":%s,\"db_error\":\"%s\",\"db_version\":\"%s\",\"db_cache_kib\":%d,"
           "\"heap_free\":%u,\"psram_size\":%u,\"psram_free\":%u,\"flash_size\":%u}",
           millis() / 1000, BUILD_TIME, accessPoint ? "access point" : "station", ip.c_str(),
           filesystemMounted ? "true" : "false", database->isOpen() ? "true" : "false", databaseError,
           databaseVersion, databaseCacheKiB,
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
           (unsigned)ESP.getFlashChipSize());

  /* The status changes every second, so it must never come from a cache. */
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", body);
}

/*
 * GET /files and GET /files/ - a page listing the downloadable files.
 *
 * The page is sent in chunks as the folder is read, one chunk per file, so
 * its size does not depend on how much memory is free.
 */
static void handleFileList()
{
  ActivityLed led;

  server->sendHeader("Cache-Control", "no-cache");
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "text/html", "");
  server->sendChunk(FILES_PAGE_HEAD);

  /*
   * The table's opening is held back until the first file turns up. An
   * empty or missing folder then produces the "No files found" line instead
   * of a table with only a header row.
   */
  bool tableOpen = false;

  /* Without a mounted filesystem there is nothing to open, and the list stays empty. */
  File dir = filesystemMounted ? LittleFS.open(FILES_DIR) : File();

  /* The folder is missing when no filesystem image with data/files was uploaded. */
  if (dir && dir.isDirectory())
  {
    /* openNextFile() returns the entries one by one and an empty File at the end. */
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile())
    {
      /* Only the files directly in the folder are listed, not subfolders. */
      if (entry.isDirectory())
        continue;

      if (!tableOpen)
      {
        server->sendChunk(FILES_TABLE_HEAD);
        tableOpen = true;
      }

      /*
       * One table row: the name as a link to /files/<name>, and the size.
       * The name is escaped because it is placed into HTML twice, once in
       * the link address and once as the visible text. If a very long name
       * makes the row exceed the buffer, snprintf cuts it short, and min()
       * keeps the length within what was actually written.
       */
      const String name = htmlEscaped(entry.name());
      char row[384];
      int length = snprintf(row, sizeof(row), "<tr><td><a href='%s/%s'>%s</a></td><td>%u</td></tr>",
                            FILES_URL, name.c_str(), name.c_str(), (unsigned)entry.size());
      server->sendChunk(row, min((size_t)length, sizeof(row) - 1));
    }
  }

  if (tableOpen)
    server->sendChunk(FILES_TABLE_END);
  else
    server->sendChunk(FILES_NO_FILES);

  server->sendChunk(FILES_PAGE_END);
  server->endChunks();
}

/*
 * Sends one file from the files folder.
 *
 * uri is the requested address, which starts with FILES_URL_SLASH, for
 * example "/files/read.me". Returns true if the file was sent, false if
 * there is no such file; the caller then answers with a 404.
 */
static bool sendFile(const String &uri)
{
  /*
   * ".." in a path means "the folder above". An address such as
   * /files/../babyname.db would otherwise reach files outside the folder,
   * including the database. Any address containing ".." is refused.
   */
  if (uri.indexOf("..") >= 0)
    return false;

  /* Without a mounted filesystem there are no files. */
  if (!filesystemMounted)
    return false;

  /*
   * Turn the address into a path on the filesystem: drop the "/files/" at
   * the front of the address and put the folder's path there instead.
   */
  String path = FILES_DIR_SLASH;
  path += uri.substring(strlen(FILES_URL_SLASH));

  /*
   * Checked first because LittleFS.open() on a missing file logs an error
   * message; exists() answers quietly.
   */
  if (!LittleFS.exists(path))
    return false;

  File file = LittleFS.open(path);

  /* A subfolder is not something that can be downloaded. */
  if (!file || file.isDirectory())
    return false;

  /*
   * The browser may reuse the file for a minute without asking again.
   * streamFile() sends the headers, including the size, and then the
   * content in blocks, so large files do not need to fit in memory.
   */
  server->sendHeader("Cache-Control", "max-age=60");
  server->streamFile(file, contentType(path));
  return true;
}

/*
 * Called for every request that no registered address matches.
 *
 * That covers three cases:
 *
 * 1. A method other than GET. Every address is registered for GET only, so
 *    a POST to an existing address also lands here. It is answered with
 *    "405 Method Not Allowed" and an Allow header naming what is accepted.
 * 2. A file below /files/. File names are not known in advance and cannot
 *    be registered one by one, so downloads are served from here.
 * 3. Everything else, which is a plain "404 Not Found".
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

  /* If the address is in the files folder but no such file exists, fall through to the 404. */
  if (uri.startsWith(FILES_URL_SLASH) && sendFile(uri))
    return;

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
  const size_t pageLength = INDEX_HTML_END - INDEX_HTML_START - 1;
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

  /*
   * The file listing. An address is matched exactly, so the forms with and
   * without the trailing slash are registered separately.
   */
  webServer.on(FILES_URL, HTTP_GET, handleFileList);
  webServer.on(FILES_URL_SLASH, HTTP_GET, handleFileList);

  /* Downloads and all errors. */
  webServer.onNotFound(handleOther);
}
