/*
 * Implementation of db_mgr; see include/db_mgr.h for what the class offers.
 *
 * The file has three parts:
 *   1. The SQL query and why it is shaped the way it is.
 *   2. ChunkWriter, a small formatter that writes JSON into a fixed buffer.
 *   3. The db_mgr methods themselves.
 */

#include "db_mgr.h"
#include <math.h>
#include <algorithm>

/*
 * The query that fetches rows.
 *
 * It depends on how tools/rebuild_db.py lays out the table: gendered_names
 * is a WITHOUT ROWID table whose primary key is (name, year, state), and
 * the name column uses the NOCASE collation. In practice that means the
 * rows are physically stored sorted by name (ignoring case), then year,
 * then state. Reading a range of names is therefore one sequential walk
 * through neighbouring rows - no separate index lookup per row, which is
 * what made the original layout about twenty times slower.
 *
 * The numbered placeholders are filled in by bindPage():
 *
 *   ?1, ?2      The first and last name of the range ("between" includes
 *               both ends).
 *   ?3, ?4, ?5  The cursor: the (name, year, state) of the last row of the
 *               previous page. "(name, year, state) > (?3, ?4, ?5)" is a
 *               row-value comparison; it keeps only rows that sort after
 *               the cursor, which is how a page continues where the last
 *               one stopped.
 *   ?6          The largest number of rows to return.
 *   ?7          The primary sex to keep, "M" or "F", or "" to keep both.
 *               "?7 = '' or ..." makes the filter switch itself off when
 *               it is empty, so one query serves both cases.
 *
 * "order by name, year, state" matches the storage order, so SQLite does
 * not sort anything; it simply reads the rows as they lie.
 */
static const char PAGE_SQL[] =
    "Select year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state "
    "from gendered_names "
    "where name between ?1 and ?2 and (name, year, state) > (?3, ?4, ?5) and (?7 = '' or primary_sex = ?7) "
    "order by name, year, state limit ?6";

/*
 * The query behind getPageAt(): a page found by its position in the range
 * instead of by a cursor. "limit ?3 offset ?4" makes SQLite read and discard
 * the first ?4 rows of the range and then return up to ?3. Discarding is
 * cheap per row but not free, so the further into the range the page lies,
 * the longer this takes; PAGE_SQL does not have that cost. ?5 is the
 * primary sex filter, as ?7 in PAGE_SQL.
 */
static const char OFFSET_SQL[] =
    "Select year, state, name, total_babies, primary_sex, primary_sex_ratio, per_100k_in_state "
    "from gendered_names "
    "where name between ?1 and ?2 and (?5 = '' or primary_sex = ?5) "
    "order by name, year, state limit ?3 offset ?4";

/*
 * The query behind getCount(): how many rows a range of names holds. It uses
 * the same "between" as PAGE_SQL, so it counts exactly the rows that paging
 * through the range would return, and the same primary sex filter (?3).
 */
static const char COUNT_SQL[] = "Select count(*) from gendered_names where name between ?1 and ?2 and (?3 = '' or primary_sex = ?3)";

/*
 * The cursor used for the first page, when there is no previous row.
 *
 * An empty name sorts before every real name, and minus infinity sorts
 * before every year, so every row in the table comes "after" this cursor.
 * Using it lets the first page run through exactly the same query as all
 * the others.
 */
static const db_mgr::Cursor BEFORE_FIRST_ROW = {"", -HUGE_VAL, ""};

/*
 * Returns a prepared statement to its reusable state when a query ends.
 *
 * An object of this type is created at the start of a query. Its destructor
 * runs when the function is left, by any path - normal return or one of the
 * early returns on error - so the cleanup can never be forgotten.
 *
 * sqlite3_reset() ends the query and releases the read lock it held.
 * sqlite3_clear_bindings() removes the bound parameters; they point at the
 * caller's strings, which stop being valid once the query function returns.
 */
struct StmtReset
{
    sqlite3_stmt *stmt;

    ~StmtReset()
    {
        /*
         * The statement is null when the database is not open. Unlike
         * sqlite3_reset(), sqlite3_clear_bindings() crashes on a null
         * pointer, so that case has to be skipped.
         */
        if (!stmt)
            return;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
};

/*
 * Writes text into a fixed buffer and passes the buffer on whenever it is
 * full.
 *
 * All output of a query goes through this class. It has two jobs:
 *
 * - Chunking. Output accumulates in the buffer; when the buffer is full it
 *   is handed to the sink (which sends it to the browser) and filling
 *   starts again from the beginning. The pieces do not line up with rows,
 *   and they do not need to: the receiver simply joins them.
 *
 * - Fast formatting. Numbers and JSON strings are produced by the small
 *   hand-written methods below instead of printf. printf is general and
 *   therefore slow, especially for floating-point values, and it needs the
 *   result to fit in one piece of buffer.
 */
class ChunkWriter
{
    /* The buffer being filled, its size, and how much of it is in use. */
    char *buffer;
    size_t capacity;
    size_t used = 0;

    /* Whether the sink has been called at least once. */
    bool emitted = false;

    /* Where full buffers go. */
    const db_mgr::ChunkSink &emit;

public:
    ChunkWriter(char *buffer, size_t capacity, const db_mgr::ChunkSink &emit)
        : buffer(buffer), capacity(capacity), emit(emit) {}

    /*
     * Hands whatever is in the buffer to the sink and empties the buffer.
     * Called automatically when the buffer fills up, and once by the owner
     * at the very end to send the last, partly filled buffer.
     */
    void flush()
    {
        if (used > 0)
        {
            emit(buffer, used);
            emitted = true;
        }
        used = 0;
    }

    /*
     * True once anything has been handed to the sink. getPage() uses this
     * to tell whether a failure can still be reported "cleanly", before any
     * output has left.
     */
    bool hasEmitted() const { return emitted; }

    /* Appends a single character, making room first if the buffer is full. */
    void put(char c)
    {
        if (used == capacity)
            flush();
        buffer[used++] = c;
    }

    /*
     * Appends length bytes of text. The text may be longer than the free
     * space, or even longer than the whole buffer: it is copied in as many
     * parts as needed, flushing between them.
     */
    void put(const char *text, size_t length)
    {
        while (length > 0)
        {
            if (used == capacity)
                flush();

            /* Copy as much as fits, then carry on with the remainder. */
            size_t part = min(length, capacity - used);
            memcpy(buffer + used, text, part);
            used += part;
            text += part;
            length -= part;
        }
    }

    /*
     * Appends a string constant, for example put("null"). The template
     * captures the array size at compile time (N counts the terminating
     * zero, hence N - 1), so no strlen() is needed at run time.
     */
    template <size_t N>
    void put(const char (&text)[N])
    {
        put(text, N - 1);
    }

    /*
     * Appends a non-negative whole number in decimal.
     *
     * The digits come out lowest first (value % 10 is the last digit), so
     * they are collected in a small array and then written in reverse. A
     * 64-bit number has at most 20 digits. The do/while loop runs at least
     * once, so the value 0 is written as "0" rather than as nothing.
     */
    void putUnsigned(uint64_t value)
    {
        char digits[20];
        size_t count = 0;

        do
        {
            digits[count++] = '0' + value % 10;
            value /= 10;
        } while (value > 0);

        while (count > 0)
            put(digits[--count]);
    }

    /*
     * Appends a whole number that may be negative.
     *
     * The negation is done in unsigned arithmetic (0 - value) on purpose:
     * negating the most negative 64-bit number as a signed value would
     * overflow, while the unsigned form gives the right magnitude for
     * every input.
     */
    void putInt(int64_t value)
    {
        if (value < 0)
        {
            put('-');
            putUnsigned(0 - (uint64_t)value);
        }
        else
        {
            putUnsigned(value);
        }
    }

    /*
     * Appends a decimal number rounded to two places, without trailing
     * zeros: 80.8 is written as "80.8", 100.0 as "100", 0.456 as "0.46".
     *
     * Two places are all the page displays, and the database holds no value
     * with more. The number is converted to a whole count of hundredths and
     * the rest is integer arithmetic, which is much cheaper than a general
     * floating-point conversion.
     */
    void putFixed2(double value)
    {
        /*
         * JSON has no way to write "not a number" or infinity, and a value
         * of 1e15 or more would overflow the integer arithmetic below.
         * Written as !(x < limit) rather than (x >= limit) because every
         * comparison with not-a-number is false: this form catches it.
         * Such values are sent as JSON null.
         */
        if (!(fabs(value) < 1e15))
        {
            put("null");
            return;
        }

        if (value < 0)
        {
            put('-');
            value = -value;
        }

        /* Adding 0.5 before cutting off the fraction rounds to the nearest hundredth. */
        uint64_t hundredths = (uint64_t)(value * 100 + 0.5);
        unsigned fraction = hundredths % 100;

        putUnsigned(hundredths / 100);

        /*
         * The fraction is 0..99. Write the tenths digit always (so 5
         * hundredths become ".05"), and the hundredths digit only when it
         * is not zero (so 80 hundredths become ".8", not ".80").
         */
        if (fraction > 0)
        {
            put('.');
            put('0' + fraction / 10);
            if (fraction % 10 > 0)
                put('0' + fraction % 10);
        }
    }

    /*
     * Appends text as a JSON string: in double quotes, with the characters
     * JSON does not allow inside a string replaced by escape sequences.
     *
     * Only three kinds of character need escaping: the double quote and the
     * backslash get a backslash in front, and control characters (below
     * 0x20, such as tab or newline) are written as \u00XX. Everything else,
     * including the bytes of non-English letters in UTF-8, passes through
     * unchanged.
     */
    void putJsonString(const char *text, size_t length)
    {
        static const char HEX_DIGITS[] = "0123456789abcdef";

        put('"');

        for (size_t i = 0; i < length; i++)
        {
            /* unsigned, so that bytes above 127 are not seen as negative and mistaken for control characters */
            unsigned char c = text[i];

            if (c == '"' || c == '\\')
            {
                put('\\');
                put(c);
            }
            else if (c < 0x20)
            {
                /* Two hexadecimal digits: the high four bits, then the low four. */
                put("\\u00");
                put(HEX_DIGITS[c >> 4]);
                put(HEX_DIGITS[c & 0x0f]);
            }
            else
            {
                put(c);
            }
        }

        put('"');
    }
};

/*
 * Writes one text column of the current row as a JSON string.
 *
 * sqlite3_column_text() must be called before sqlite3_column_bytes(): the
 * first call may convert the value to text, and only then is the byte count
 * correct. A NULL value comes back as a null pointer with a length of 0 and
 * is written as an empty string.
 */
static void putTextColumn(ChunkWriter &out, sqlite3_stmt *stmt, int column)
{
    const char *text = (const char *)sqlite3_column_text(stmt, column);
    out.putJsonString(text ? text : "", sqlite3_column_bytes(stmt, column));
}

/*
 * Writes the row the statement is currently on as a JSON array, for example
 *
 *   [2015,"US","Bob",14,"M",100,0.4]
 *
 * The column numbers are positions in the select list of PAGE_SQL:
 * 0 year, 1 state, 2 name, 3 total_babies, 4 primary_sex,
 * 5 primary_sex_ratio, 6 per_100k_in_state. The script in web/index.html
 * relies on this order.
 */
static void writeRow(ChunkWriter &out, sqlite3_stmt *stmt)
{
    out.put('[');
    out.putInt(sqlite3_column_int64(stmt, 0));
    out.put(',');
    putTextColumn(out, stmt, 1);
    out.put(',');
    putTextColumn(out, stmt, 2);
    out.put(',');
    out.putInt(sqlite3_column_int64(stmt, 3));
    out.put(',');
    putTextColumn(out, stmt, 4);
    out.put(',');
    out.putFixed2(sqlite3_column_double(stmt, 5));
    out.put(',');
    out.putFixed2(sqlite3_column_double(stmt, 6));
    out.put(']');
}

db_mgr::~db_mgr()
{
    close();
}

bool db_mgr::open(const char *filename, int cacheKiB)
{
    /* Start from a clean state, so calling open() twice does not leak the first connection. */
    close();

    /*
     * The SQLite build in the Sqlite3Esp32 library is compiled with
     * SQLITE_OMIT_AUTOINIT, so it does not initialise itself on first use
     * and this call is required. Calling it again later does nothing.
     */
    sqlite3_initialize();

    /*
     * Open read-only. The application never changes the database, and in
     * this mode SQLite creates no journal file and does no write locking,
     * which saves flash wear and time.
     */
    if (sqlite3_open_v2(filename, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        /*
         * Even a failed open normally returns a connection object, whose
         * only purpose is to carry the error message. It is null only when
         * there was not even enough memory to create it.
         */
        if (db)
            setErrorFromDb();
        else
            setError("Out of memory");
        close();
        return false;
    }

    /*
     * Enlarge the page cache. SQLite reads the file in pages (4096 bytes in
     * this database) and keeps recently used ones in memory. The library's
     * default cache is 1 KiB - less than one page - so without this every
     * access goes back to flash. A negative cache_size is how SQLite is
     * told "this many KiB" (a positive number would mean a count of pages).
     */
    if (cacheKiB > 0)
    {
        char pragma[40];
        snprintf(pragma, sizeof(pragma), "PRAGMA cache_size=-%d", cacheKiB);

        if (sqlite3_exec(db, pragma, nullptr, nullptr, nullptr) != SQLITE_OK)
        {
            setErrorFromDb();
            close();
            return false;
        }
    }

    /*
     * Compile the three queries now, once. Parsing and planning SQL is
     * expensive compared with running these queries; afterwards each request
     * only binds new values and steps through rows.
     *
     * sizeof(PAGE_SQL) passes the length including the terminating zero,
     * which lets SQLite skip measuring the string. SQLITE_PREPARE_PERSISTENT
     * tells SQLite the statement will be kept for a long time, so it stores
     * it in ordinary memory rather than its small short-term pool.
     *
     * This step also fails if the file is not a database or lacks the
     * expected table, which is how a wrong or empty file is detected.
     */
    if (sqlite3_prepare_v3(db, PAGE_SQL, sizeof(PAGE_SQL), SQLITE_PREPARE_PERSISTENT, &pageStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v3(db, OFFSET_SQL, sizeof(OFFSET_SQL), SQLITE_PREPARE_PERSISTENT, &offsetStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v3(db, COUNT_SQL, sizeof(COUNT_SQL), SQLITE_PREPARE_PERSISTENT, &countStmt, nullptr) != SQLITE_OK)
    {
        setErrorFromDb();
        close();
        return false;
    }

    return true;
}

/*
 * The statements have to be finalized before the connection is closed;
 * SQLite refuses to close a connection that still has statements. These
 * functions accept a null pointer and then do nothing, so close() is safe
 * on a database that is closed or only half opened.
 */
void db_mgr::close()
{
    dropSortedRange();
    sqlite3_finalize(pageStmt);
    sqlite3_finalize(offsetStmt);
    sqlite3_finalize(countStmt);
    sqlite3_close(db);
    pageStmt = nullptr;
    offsetStmt = nullptr;
    countStmt = nullptr;
    db = nullptr;
}

/* strlcpy copies at most the size of the array and always terminates the result. */
void db_mgr::setError(const char *message)
{
    strlcpy(error, message, sizeof(error));
}

/*
 * sqlite3_errmsg() returns text owned by the connection that later calls
 * overwrite, which is why it is copied into our own array straight away.
 */
void db_mgr::setErrorFromDb()
{
    setError(sqlite3_errmsg(db));
}

bool db_mgr::bindPage(const char *from, const char *to, const char *sex, const Cursor *after, int limit)
{
    /* Without a connection there is no statement to bind to. */
    if (!db)
    {
        setError("Database is not open");
        return false;
    }

    /* No cursor means the first page: continue after "before everything". */
    if (!after)
        after = &BEFORE_FIRST_ROW;

    /*
     * Choose where reading starts.
     *
     * SQLite uses the lower bound of "name between ?1 and ?2" to jump
     * directly to the first matching row. If that bound were always the
     * start of the range, a later page would begin at the start again and
     * discard every row before the cursor - cheap per row, but growing with
     * each page. Passing the cursor's name as the lower bound instead makes
     * every page start right where the previous one ended.
     *
     * The cursor's name is only used when it lies beyond the start of the
     * range. strcasecmp compares without regard to case, matching the
     * NOCASE ordering of the name column.
     */
    const char *first = strcasecmp(after->name, from) > 0 ? after->name : from;

    /*
     * Fill in the seven placeholders of PAGE_SQL.
     *
     * For the text values, -1 means "the string is null-terminated, measure
     * it", and SQLITE_STATIC promises that the string stays valid until the
     * query is finished, so SQLite uses it in place instead of making a
     * copy. That promise is why getPage() requires its arguments to stay
     * valid until it returns.
     *
     * The limit is bound as limit + 1: asking for one row more than will be
     * shown is how writeRows() learns whether another page follows.
     */
    if (sqlite3_bind_text(pageStmt, 1, first, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(pageStmt, 2, to, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(pageStmt, 3, after->name, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_double(pageStmt, 4, after->year) != SQLITE_OK ||
        sqlite3_bind_text(pageStmt, 5, after->state, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int(pageStmt, 6, limit + 1) != SQLITE_OK ||
        sqlite3_bind_text(pageStmt, 7, sex, -1, SQLITE_STATIC) != SQLITE_OK)
    {
        setErrorFromDb();
        return false;
    }

    return true;
}

/*
 * Fills in the five placeholders of OFFSET_SQL: the two bounds of the range,
 * the row limit (again one more than will be shown, see bindPage()), the
 * number of rows to skip, and the primary sex filter.
 */
bool db_mgr::bindOffsetPage(const char *from, const char *to, const char *sex, long offset, int limit)
{
    if (!db)
    {
        setError("Database is not open");
        return false;
    }

    if (sqlite3_bind_text(offsetStmt, 1, from, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(offsetStmt, 2, to, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int(offsetStmt, 3, limit + 1) != SQLITE_OK ||
        sqlite3_bind_int64(offsetStmt, 4, offset) != SQLITE_OK ||
        sqlite3_bind_text(offsetStmt, 5, sex, -1, SQLITE_STATIC) != SQLITE_OK)
    {
        setErrorFromDb();
        return false;
    }

    return true;
}

/*
 * Steps through a statement whose parameters are already bound and writes
 * its rows, separated by commas. Both page queries return the same columns,
 * so this works for either.
 */
bool db_mgr::writeRows(ChunkWriter &out, sqlite3_stmt *stmt, int limit, bool &more)
{
    int count = 0;
    int result_code;

    /*
     * Each sqlite3_step() advances to the next row and returns SQLITE_ROW,
     * or returns SQLITE_DONE when there are no more rows, or an error code.
     */
    while ((result_code = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        /*
         * The query was asked for limit + 1 rows. Reaching that extra row
         * proves that more data exists beyond this page. It is not written:
         * it will be the first row of the next page.
         */
        if (count == limit)
        {
            more = true;
            return true;
        }

        /* Rows are separated by commas, so every row except the first gets one in front. */
        if (count++ > 0)
            out.put(',');

        writeRow(out, stmt);
    }

    /* The loop ended without SQLITE_DONE, so the last step failed. */
    if (result_code != SQLITE_DONE)
    {
        setErrorFromDb();
        return false;
    }

    return true;
}

long db_mgr::getCount(const char *from, const char *to, const char *sex)
{
    /* However this function is left, the statement is made reusable again (see StmtReset). */
    StmtReset reset{countStmt};

    if (!db)
    {
        setError("Database is not open");
        return -1;
    }

    /* The two bounds and the filter, used in place and not copied, exactly as in bindPage(). */
    if (sqlite3_bind_text(countStmt, 1, from, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(countStmt, 2, to, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(countStmt, 3, sex, -1, SQLITE_STATIC) != SQLITE_OK)
    {
        setErrorFromDb();
        return -1;
    }

    /* count(*) always produces exactly one row, so anything else is a failure. */
    if (sqlite3_step(countStmt) != SQLITE_ROW)
    {
        setErrorFromDb();
        return -1;
    }

    return (long)sqlite3_column_int64(countStmt, 0);
}

/*
 * Writes the whole JSON document for one page: the opening, the rows of the
 * given statement, the "more" flag and, if something went wrong, the error.
 *
 * bound says whether binding the statement's parameters succeeded. It is
 * passed in, rather than checked by the caller, so that a binding failure
 * takes exactly the same path as a failure while reading rows.
 */
bool db_mgr::writePage(sqlite3_stmt *stmt, bool bound, int limit, const ChunkSink &emit)
{
    ChunkWriter out(chunk, CHUNK_SIZE, emit);
    bool more = false;

    /* The opening of the document. It only goes into the buffer; nothing is emitted yet. */
    out.put("{\"rows\":[");

    bool ok = bound && writeRows(out, stmt, limit, more);

    /*
     * A failure before anything was emitted (database not open, a bad
     * binding, or an error within the first buffer-full of rows): the
     * buffer is simply abandoned and the caller is told. Since no output
     * has left, the caller can still answer with a proper error of its own.
     */
    if (!ok && !out.hasEmitted())
        return false;

    /* Close the array of rows and say whether another page follows. */
    if (more)
        out.put("],\"more\":true");
    else
        out.put("],\"more\":false");

    /*
     * A failure after output had already been emitted. The start of the
     * document cannot be taken back, so it is finished as valid JSON with
     * the error added, where the reader will find it.
     */
    if (!ok)
    {
        out.put(",\"error\":");
        out.putJsonString(error, strlen(error));
    }

    /* Close the document and send the final, partly filled buffer. */
    out.put('}');
    out.flush();

    return ok;
}

bool db_mgr::getPage(const char *from, const char *to, const char *sex, const Cursor *after, int limit, const ChunkSink &emit)
{
    /* A limit below 1 would return nothing and could never make progress, so 1 is the minimum. */
    limit = max(limit, 1);

    /*
     * From here on, however this function is left, the statement is reset
     * and its bindings are cleared (see StmtReset).
     */
    StmtReset reset{pageStmt};

    return writePage(pageStmt, bindPage(from, to, sex, after, limit), limit, emit);
}

bool db_mgr::getPageAt(const char *from, const char *to, const char *sex, long offset, int limit, const ChunkSink &emit)
{
    limit = max(limit, 1);

    StmtReset reset{offsetStmt};

    /* A negative position makes no sense; treat it as the start of the range. */
    return writePage(offsetStmt, bindOffsetPage(from, to, sex, max(offset, 0L), limit), limit, emit);
}

/* ------------------------------------------------------------------ */
/* Sorted pages                                                       */
/* ------------------------------------------------------------------ */

/*
 * The most memory a kept range may take on a board without PSRAM. There the
 * memory comes from the small internal heap, which the network also lives
 * on, so only modest ranges are sorted. With PSRAM there is no fixed limit;
 * the range is refused only if the memory cannot be had.
 */
static const size_t SORT_BYTES_WITHOUT_PSRAM = 48 * 1024;

/* Which columns hold text. All others hold numbers. */
static bool isTextColumn(int column)
{
    return column == 1 || column == 2 || column == 4;
}

/*
 * Changes the size of a block of memory for the kept range, in PSRAM when
 * the board has it. Like realloc(): a null pointer allocates, and on
 * failure null is returned and the old block is untouched.
 */
static void *sortMemory(void *block, size_t size)
{
    return psramFound() ? ps_realloc(block, size) : realloc(block, size);
}

/*
 * Finds where a column's value starts inside the JSON text of one row,
 * for example inside [2015,"US","Mar",12,"F",100,0.3].
 *
 * The values before it are stepped over one by one. A text value has to be
 * walked through to its closing quote, minding that a backslash makes the
 * next character part of the text; a number simply ends at the comma.
 */
static const char *columnValue(const char *row, int column)
{
    const char *p = row + 1;

    for (int i = 0; i < column; i++)
    {
        if (*p == '"')
        {
            for (p++; *p != '"'; p++)
            {
                if (*p == '\\')
                    p++;
            }
        }

        while (*p != ',')
            p++;

        p++;
    }

    return p;
}

void db_mgr::dropSortedRange()
{
    free(sortedRows);
    free(sortedEntries);
    sortedRows = nullptr;
    sortedEntries = nullptr;
    sortedBytes = 0;
    sortedCount = 0;
    sortedColumn = -1;
    sortedValid = false;
}

/*
 * Reads the whole range into memory, unless it is the one already there.
 *
 * Each row is turned into its JSON text by the same writeRow() that
 * produces normal pages, so a sorted page looks exactly like any other.
 * The text is collected through a ChunkWriter whose "sink" appends to
 * sortedRows instead of sending anything; flushing after every row tells
 * where one row ends and the next begins.
 */
bool db_mgr::loadSortedRange(const char *from, const char *to, const char *sex)
{
    if (sortedValid && sortedFrom == from && sortedTo == to && sortedSex == sex)
        return true;

    dropSortedRange();

    StmtReset reset{offsetStmt};

    /*
     * The position query, from the start of the range and without a limit:
     * bindOffsetPage() asks for limit + 1 rows, and -1 means "all" to SQLite.
     */
    if (!bindOffsetPage(from, to, sex, 0, -2))
        return false;

    size_t rowCapacity = 0;
    uint32_t entryCapacity = 0;
    bool enoughMemory = true;

    /* Appends a piece of text to sortedRows, doubling the block when it is full. */
    const ChunkSink append = [&](const char *data, size_t length)
    {
        if (!enoughMemory)
            return;

        if (sortedBytes + length > rowCapacity)
        {
            const size_t wanted = max(max(rowCapacity * 2, (size_t)8192), sortedBytes + length);
            char *grown = (!psramFound() && wanted > SORT_BYTES_WITHOUT_PSRAM) ? nullptr : (char *)sortMemory(sortedRows, wanted);

            if (!grown)
            {
                enoughMemory = false;
                return;
            }

            sortedRows = grown;
            rowCapacity = wanted;
        }

        memcpy(sortedRows + sortedBytes, data, length);
        sortedBytes += length;
    };

    ChunkWriter out(chunk, CHUNK_SIZE, append);
    int result_code = SQLITE_DONE;

    while (enoughMemory && (result_code = sqlite3_step(offsetStmt)) == SQLITE_ROW)
    {
        if (sortedCount == entryCapacity)
        {
            const uint32_t wanted = max(entryCapacity * 2, (uint32_t)256);
            SortEntry *grown = (SortEntry *)sortMemory(sortedEntries, wanted * sizeof(SortEntry));

            if (!grown)
            {
                enoughMemory = false;
                break;
            }

            sortedEntries = grown;
            entryCapacity = wanted;
        }

        const size_t start = sortedBytes;
        writeRow(out, offsetStmt);
        out.flush();

        sortedEntries[sortedCount].offset = start;
        sortedEntries[sortedCount].length = sortedBytes - start;
        sortedCount++;
    }

    if (!enoughMemory)
    {
        dropSortedRange();
        setError("Not enough memory to sort this many rows");
        return false;
    }

    if (result_code != SQLITE_DONE)
    {
        setErrorFromDb();
        dropSortedRange();
        return false;
    }

    sortedFrom = from;
    sortedTo = to;
    sortedSex = sex;
    sortedValid = true;
    return true;
}

/*
 * Orders the entries of the kept range by one column.
 *
 * First every entry gets its key: the row's value in that column, read back
 * out of the row's JSON text. Then the entries are sorted by key. Rows with
 * equal keys are ordered by their position in sortedRows, which is name
 * order, because that is how they were read.
 *
 * The name column needs no keys at all: name order is position order.
 */
void db_mgr::sortKeptRange(int column)
{
    if (sortedColumn == column)
        return;

    const bool text = isTextColumn(column);

    for (uint32_t i = 0; i < sortedCount && column != 2; i++)
    {
        SortEntry &entry = sortedEntries[i];
        const char *value = columnValue(sortedRows + entry.offset, column);

        if (text)
        {
            /*
             * Pack up to eight bytes of the text, first byte highest, so
             * that the integers compare the way the texts do. A value that
             * is not text (null) gets the lowest key.
             */
            uint64_t packed = 0;

            if (*value == '"')
            {
                value++;
                for (int byte = 0; byte < 8; byte++)
                {
                    packed <<= 8;
                    if (*value != '"')
                        packed |= (uint8_t)*value++;
                }
            }

            entry.key.text = packed;
        }
        else
        {
            /* A number ends at the comma or bracket after it, which is where strtod() stops. null sorts first. */
            entry.key.number = (*value == '-' || (*value >= '0' && *value <= '9')) ? strtod(value, nullptr) : -INFINITY;
        }
    }

    if (column == 2)
    {
        std::sort(sortedEntries, sortedEntries + sortedCount,
                  [](const SortEntry &a, const SortEntry &b) { return a.offset < b.offset; });
    }
    else if (text)
    {
        std::sort(sortedEntries, sortedEntries + sortedCount,
                  [](const SortEntry &a, const SortEntry &b)
                  { return a.key.text != b.key.text ? a.key.text < b.key.text : a.offset < b.offset; });
    }
    else
    {
        std::sort(sortedEntries, sortedEntries + sortedCount,
                  [](const SortEntry &a, const SortEntry &b)
                  { return a.key.number != b.key.number ? a.key.number < b.key.number : a.offset < b.offset; });
    }

    sortedColumn = column;
}

bool db_mgr::getSortedPage(const char *from, const char *to, const char *sex, int column, bool descending, long offset, int limit, const ChunkSink &emit)
{
    if (!db)
    {
        setError("Database is not open");
        return false;
    }

    if (column < 0 || column >= COLUMN_COUNT)
    {
        setError("No such column to sort by");
        return false;
    }

    /* Nothing has been emitted yet, so on failure the caller can still answer in its own way. */
    if (!loadSortedRange(from, to, sex))
        return false;

    sortKeptRange(column);

    /* The page: rows first..last-1 of the sorted order, counted from the far end when descending. */
    const uint32_t first = min((uint32_t)max(offset, 0L), sortedCount);
    const uint32_t last = min(first + (uint32_t)max(limit, 1), sortedCount);

    ChunkWriter out(chunk, CHUNK_SIZE, emit);
    out.put("{\"rows\":[");

    for (uint32_t i = first; i < last; i++)
    {
        const SortEntry &entry = sortedEntries[descending ? sortedCount - 1 - i : i];

        if (i > first)
            out.put(',');

        out.put(sortedRows + entry.offset, entry.length);
    }

    if (last < sortedCount)
        out.put("],\"more\":true}");
    else
        out.put("],\"more\":false}");

    out.flush();
    return true;
}
