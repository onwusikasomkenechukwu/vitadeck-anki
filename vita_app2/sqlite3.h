// Minimal sqlite3.h shim for the Vita reviewer.
//
// The Vita's system SQLite (SCE_SYSMODULE_SQLITE / libSceSqlite_stub) exposes
// the full standard sqlite3_* C API as exported symbols, but vitasdk does
// NOT ship the public sqlite3.h header. Rather than bundle the 700KB
// amalgamation (sqlite3.c + sqlite3.h) or pull it over the network, we
// declare only the functions we actually call, with signatures matching
// the canonical SQLite 3 ABI exactly so the call sites stay portable.
//
// Reference: https://sqlite.org/c3ref/intro.html — function prototypes
// taken from there. Inferring binary compatibility from the export list
// of libSceSqlite_stub.a + standard sqlite3 ABI conventions; if any call
// returns garbage on the device, this is the first place to look.

#ifndef VITADECK_SQLITE3_SHIM_H
#define VITADECK_SQLITE3_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;

#define SQLITE_OK    0
#define SQLITE_ROW   100
#define SQLITE_DONE  101

int  sqlite3_open(const char *filename, sqlite3 **ppDb);
int  sqlite3_close(sqlite3 *db);
const char *sqlite3_errmsg(sqlite3 *db);

int  sqlite3_prepare_v2(sqlite3 *db,
                        const char *zSql, int nByte,
                        sqlite3_stmt **ppStmt, const char **pzTail);
int  sqlite3_step(sqlite3_stmt *stmt);
int  sqlite3_finalize(sqlite3_stmt *stmt);

const unsigned char *sqlite3_column_text(sqlite3_stmt *stmt, int iCol);
int                  sqlite3_column_bytes(sqlite3_stmt *stmt, int iCol);
sqlite3_int64        sqlite3_column_int64(sqlite3_stmt *stmt, int iCol);

int  sqlite3_reset(sqlite3_stmt *stmt);
int  sqlite3_bind_int(sqlite3_stmt *stmt, int iParam, int v);
int  sqlite3_bind_int64(sqlite3_stmt *stmt, int iParam, sqlite3_int64 v);

#ifdef __cplusplus
}
#endif

#endif  // VITADECK_SQLITE3_SHIM_H
