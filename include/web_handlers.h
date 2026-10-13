/*
 * The web side of the application: which addresses exist and what each one
 * answers. The implementation is in src/web_handlers.cpp.
 */

#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "chunk_server.h"
#include "db_mgr.h"

/*
 * Registers every address the application answers on the given server:
 *
 *   /             the application page (search form and results)
 *   /query_db     the same page, kept so old links still work
 *   /api/names    one page of names from the database, as JSON
 *   /api/count    how many names a range holds, as JSON
 *   /api/status   the state of the device, as JSON
 *   /files, /files/         a listing of the downloadable files
 *   /files/<name>           one downloadable file
 *   anything else           a 404 (or 405 for a method other than GET)
 *
 * Call it once from setup(), after the attempt to mount the filesystem (the
 * handlers read the database file's size and date from it) and before
 * server.begin().
 *
 * server and db must outlive the server; the handlers keep pointers to them.
 * cacheKiB is the database cache size chosen at startup; it is not used for
 * anything here except being reported by /api/status.
 * fsMounted says whether the filesystem could be mounted. When it could
 * not, the handlers leave the filesystem alone and /api/status reports it.
 */
void registerWebHandlers(ChunkServer &server, db_mgr &db, int cacheKiB, bool fsMounted);

#endif
