/*
 * db_mgr - read-only access to the baby names database.
 *
 * The class hides SQLite completely. A caller opens the database once and
 * then asks for "pages" of names; each page arrives as ready-made JSON text,
 * handed over in pieces through a callback so it can be streamed straight
 * to a browser without ever being held in memory as a whole.
 *
 * It is written for a microcontroller:
 *
 * - The SQL queries are compiled once, in open(), and reused for every request.
 * - Rows are written into a fixed buffer that is part of the object, so a
 *   query allocates no memory of its own and cannot fragment the heap.
 * - The one exception is sorting by a column, which keeps a copy of the
 *   searched range in memory (see getSortedPage()).
 * - Paging continues from a "cursor" (the last row already shown) instead
 *   of skipping rows, so page 100 costs the same as page 1.
 *
 * The matching database file is produced by tools/rebuild_db.py.
 */

#ifndef DB_MGR_H
#define DB_MGR_H

#include <Arduino.h>
#include <functional>
#include <sqlite3.h>

/*
 * The helper that formats output into the buffer. It is only needed inside
 * db_mgr.cpp; this forward declaration lets a private method below take a
 * reference to it without exposing the class here.
 */
class ChunkWriter;

class db_mgr
{
public:
    /*
     * The callback that receives query output.
     *
     * It is called once for every buffer-full of JSON text, in order. data
     * is not null-terminated; exactly length bytes are valid, and only for
     * the duration of the call, because the buffer is reused afterwards.
     */
    using ChunkSink = std::function<void(const char *data, size_t length)>;

    /*
     * The size of the output buffer, and so the largest piece a ChunkSink
     * ever receives. 1400 bytes plus the web server's chunk framing still
     * fit in one TCP packet (see ChunkServer::MAX_CHUNK_SIZE).
     */
    static const size_t CHUNK_SIZE = 1400;

    /*
     * A position in the table: the key of one row.
     *
     * The table is ordered by (name, year, state) and that combination is
     * unique, so these three values identify exactly one row. A page that
     * "continues after" a cursor starts with the row that follows it.
     *
     * year is a double because that is how it arrives from the web request;
     * SQLite compares it numerically with the stored value either way.
     */
    struct Cursor
    {
        const char *name;
        double year;
        const char *state;
    };

    db_mgr() = default;

    /* Closes the database if it is still open. */
    ~db_mgr();

    /*
     * Copying is forbidden. Two objects holding the same SQLite connection
     * would both try to close it.
     */
    db_mgr(const db_mgr &) = delete;
    db_mgr &operator=(const db_mgr &) = delete;

    /*
     * Opens the database file read-only and prepares the queries.
     *
     * filename is the path as the C library sees it, including the mount
     * point, for example "/littlefs/babyname.db".
     *
     * cacheKiB is the size of SQLite's page cache in KiB. Pass 0 to keep
     * the library's default, which is a single KiB and effectively no
     * cache. Only pass a large value when the memory is really available
     * (in this project: when the board has PSRAM).
     *
     * Returns true on success. On failure the database is left closed and
     * lastError() describes the problem. An already open database is
     * closed first, so open() may be called again.
     */
    bool open(const char *filename, int cacheKiB = 0);

    /* Releases the queries and the connection. Safe to call when already closed. */
    void close();

    bool isOpen() const { return db != nullptr; }

    /*
     * Produces one page of a name range as a JSON document:
     *
     *   {"rows":[[year,state,name,total_babies,primary_sex,ratio,per_100k],...],"more":false}
     *
     * from, to   The first and last name of the range, both included. Names
     *            are compared without regard to case, so "bob" finds "Bob".
     * after      The last row of the previous page, or nullptr for the first
     *            page.
     * limit      The largest number of rows to return (at least 1).
     * emit       Receives the document, one buffer-full at a time.
     *
     * "more" in the document is true when rows remain beyond this page. The
     * caller then asks for the next page with the last row it received as
     * the cursor.
     *
     * Returns true on success. On failure it returns false, lastError()
     * holds the reason, and one of two things has happened:
     *
     * - The failure came before any output was emitted. Then nothing is
     *   emitted at all, so the caller is free to answer in its own way (the
     *   web handler sends an HTTP error status).
     * - The failure came after output had already been emitted. Then the
     *   document is still completed properly and carries an extra "error"
     *   member, so whoever reads it gets valid JSON and sees the problem.
     *
     * from, to and the strings in the cursor are used in place, not copied.
     * They must stay valid until getPage() returns.
     */
    bool getPage(const char *from, const char *to, const Cursor *after, int limit, const ChunkSink &emit);

    /*
     * Produces the page that starts at a given position in the range: the
     * same document as getPage(), but found by counting rows instead of by
     * a cursor. offset is the number of rows of the range that come before
     * the page, so 0 is the first page.
     *
     * This is what makes it possible to jump straight to a page whose cursor
     * is not known, such as the last one. The price is that the rows before
     * the page are read and discarded, so the cost grows with offset. To move
     * to the next page, getPage() with a cursor is the cheaper choice.
     *
     * Everything else - the document, the return value, the lifetime of the
     * strings - is as described for getPage().
     */
    bool getPageAt(const char *from, const char *to, long offset, int limit, const ChunkSink &emit);

    /* How many columns a row has, and so how many there are to sort by. */
    static const int COLUMN_COUNT = 7;

    /*
     * Produces a page of the range in a different order: sorted by one of
     * the columns instead of by name. The document is the same as getPage()'s.
     *
     * column      Which column to sort by, as its position in a row: 0 is
     *             the year, 1 the state, and so on up to COLUMN_COUNT - 1.
     * descending  true for largest first. Rows with equal values are put in
     *             name order, reversed as well when descending, so the
     *             descending order is exactly the ascending one backwards.
     * offset      The number of rows of the sorted range before the page.
     *
     * SQLite is not asked to sort. On this board its sorter is slow, and
     * for a large range it writes temporary files, which the filesystem
     * setup here cannot take. Instead the whole range is read once, in name
     * order, and kept in memory as ready-made JSON rows; the sorting is done
     * here, and every page is copied out of that memory.
     *
     * The cost is therefore uneven:
     *
     * - The first sorted request for a range reads all of its rows: a
     *   fraction of a second for a few thousand, several seconds for the
     *   whole table.
     * - A different column for the same range only sorts again.
     * - Another page, or the other direction, costs next to nothing.
     *
     * One range is kept at a time, until a different one is sorted. It takes
     * about 50 bytes per row. If that much memory cannot be had, the method
     * fails with a message saying so, before any output.
     *
     * The decimal columns are compared as they are shown, rounded to two
     * places, and text columns by their first eight bytes.
     *
     * Everything else is as described for getPage().
     */
    bool getSortedPage(const char *from, const char *to, int column, bool descending, long offset, int limit, const ChunkSink &emit);

    /*
     * Counts the names in a range, with the same meaning of from and to as
     * in getPage().
     *
     * Returns the number of rows, or -1 on failure, in which case
     * lastError() holds the reason.
     *
     * Counting reads every row of the range, so its cost grows with the
     * size of the range: a few milliseconds for a few hundred rows, around
     * half a second for the whole table.
     */
    long getCount(const char *from, const char *to);

    /*
     * A description of the most recent failure. Only meaningful right after
     * a method reported one; it is not cleared by later successes.
     */
    const char *lastError() const { return error; }

private:
    /* The SQLite connection, or nullptr while the database is closed. */
    sqlite3 *db = nullptr;

    /* The compiled paging query, prepared in open() and reused by every getPage(). */
    sqlite3_stmt *pageStmt = nullptr;

    /* The compiled query for a page found by position, used by getPageAt(). */
    sqlite3_stmt *offsetStmt = nullptr;

    /*
     * One row of the range kept for sorting: where its JSON text is in
     * sortedRows, how long it is, and its value in the column currently
     * sorted by. The value is a number or, for a text column, the first
     * eight bytes of the text packed into an integer so that comparing the
     * integers compares the texts.
     */
    struct SortEntry
    {
        union
        {
            double number;
            uint64_t text;
        } key;
        uint32_t offset;
        uint32_t length;
    };

    /*
     * The range kept for sorting (see getSortedPage()).
     *
     * sortedRows holds the JSON text of all its rows one after another, in
     * name order. sortedEntries has one entry per row and is what gets
     * sorted; the text itself never moves. sortedColumn is the column the
     * entries are currently ordered by, or -1 right after loading.
     */
    bool sortedValid = false;
    String sortedFrom;
    String sortedTo;
    char *sortedRows = nullptr;
    size_t sortedBytes = 0;
    SortEntry *sortedEntries = nullptr;
    uint32_t sortedCount = 0;
    int sortedColumn = -1;

    /* The compiled counting query, prepared in open() and reused by every getCount(). */
    sqlite3_stmt *countStmt = nullptr;

    /*
     * The text returned by lastError(). A fixed array rather than a String,
     * so reporting an error never needs to allocate memory - which matters
     * most when the error is that memory ran out.
     */
    char error[96] = "";

    /*
     * The output buffer handed to the ChunkSink. Keeping it in the object
     * costs CHUNK_SIZE bytes of permanent memory but keeps it off the stack,
     * where SQLite itself already needs a lot of room.
     */
    char chunk[CHUNK_SIZE];

    /* Fills in the parameters of the cursor query for one page. */
    bool bindPage(const char *from, const char *to, const Cursor *after, int limit);

    /* Fills in the parameters of the position query for one page. */
    bool bindOffsetPage(const char *from, const char *to, long offset, int limit);

    /* Steps through an already bound statement and writes its rows; sets more when rows remain. */
    bool writeRows(ChunkWriter &out, sqlite3_stmt *stmt, int limit, bool &more);

    /* Writes the complete JSON document for one page from an already bound statement. */
    bool writePage(sqlite3_stmt *stmt, bool bound, int limit, const ChunkSink &emit);

    /* Makes the given range the one kept for sorting, reading it if it is not already. */
    bool loadSortedRange(const char *from, const char *to);

    /* Orders the kept range by a column, unless it already is. */
    void sortKeptRange(int column);

    /* Releases the kept range. */
    void dropSortedRange();

    /* Stores a message for lastError(), cutting it short if it is too long. */
    void setError(const char *message);

    /* Stores SQLite's own description of what just went wrong. */
    void setErrorFromDb();
};

#endif
