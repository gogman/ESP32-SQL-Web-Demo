/*
 * The web side of the application: which addresses exist and what each one
 * answers. The implementation is in src/web_handlers.cpp.
 */

#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "db_mgr.h"
#include <ESPAsyncWebServer.h>

/*
 * Registers every address the application answers on the given server:
 *
 *   /             the application page (search form and results)
 *   /query_db     the same page, kept so old links still work
 *   /api/names    one page of names from the database, as JSON
 *   /api/count    how many names a range holds, as JSON
 *   /api/status   the state of the device, as JSON
 *   /setup/       the setup page, where the network settings are changed
 *                 (/setup without the slash is sent on to it)
 *   /api/settings the network settings: GET reads them as JSON, POST saves
 *                 them and restarts the board
 *   /bootstrap.min.css  the Bootstrap stylesheet, gzip-compressed
 *   /favicon.ico  the page's icon
 *   anything else the 404 answer (or 405 for a method that is not accepted)
 *
 * Call it once from setup(), after the attempt to mount the filesystem (the
 * handlers read the database file's size and date from it) and before
 * server.begin().
 *
 * server and db must outlive the registered handlers; the handlers keep a
 * pointer to db.
 * cacheKiB is the database cache size chosen at startup; it is not used for
 * anything here except being reported by /api/status.
 * fsMounted says whether the filesystem could be mounted. When it could
 * not, the handlers leave the filesystem alone and /api/status reports it.
 */
void registerWebHandlers(AsyncWebServer &server, db_mgr &db, int cacheKiB, bool fsMounted);

// Switches the request LED off once its blink time has passed. Call from loop().
void serviceActivityLed();

#endif
