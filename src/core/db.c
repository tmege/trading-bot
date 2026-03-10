#include "core/db.h"
#include "core/logging.h"
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>

static const char *SCHEMA_SQL =
    /* Trade history */
    "CREATE TABLE IF NOT EXISTS trades ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  timestamp_ms  INTEGER NOT NULL,"
    "  coin          TEXT NOT NULL,"
    "  side          TEXT NOT NULL,"
    "  price         TEXT NOT NULL,"
    "  size          TEXT NOT NULL,"
    "  fee           TEXT,"
    "  pnl           TEXT,"
    "  oid           INTEGER,"
    "  tid           INTEGER,"
    "  strategy      TEXT,"
    "  cloid         TEXT"
    ");"

    /* Current positions snapshot */
    "CREATE TABLE IF NOT EXISTS positions ("
    "  coin          TEXT PRIMARY KEY,"
    "  size          TEXT NOT NULL,"
    "  entry_px      TEXT NOT NULL,"
    "  unrealized_pnl TEXT,"
    "  leverage      INTEGER,"
    "  updated_ms    INTEGER"
    ");"

    /* Daily P&L summary */
    "CREATE TABLE IF NOT EXISTS daily_pnl ("
    "  date          TEXT PRIMARY KEY,"
    "  realized_pnl  TEXT NOT NULL,"
    "  unrealized_pnl TEXT,"
    "  fees_paid     TEXT,"
    "  n_trades      INTEGER"
    ");"

    /* Lua strategy persisted state */
    "CREATE TABLE IF NOT EXISTS strategy_state ("
    "  strategy_name TEXT PRIMARY KEY,"
    "  state_json    TEXT,"
    "  updated_ms    INTEGER"
    ");"

    /* AI advisory call log */
    "CREATE TABLE IF NOT EXISTS advisory_log ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  timestamp_ms  INTEGER NOT NULL,"
    "  prompt_text   TEXT,"
    "  response_text TEXT,"
    "  adjustments   TEXT"
    ");"

    /* Indices for common queries */
    "CREATE INDEX IF NOT EXISTS idx_trades_ts ON trades(timestamp_ms);"
    "CREATE INDEX IF NOT EXISTS idx_trades_coin ON trades(coin);"
    "CREATE INDEX IF NOT EXISTS idx_trades_strategy ON trades(strategy);";

int tb_db_open(sqlite3 **db, const char *db_path) {
    /* Ensure parent directory exists */
    char path_copy[512];
    snprintf(path_copy, sizeof(path_copy), "%s", db_path);
    char *dir = dirname(path_copy);
    mkdir(dir, 0700);

    int rc = sqlite3_open_v2(db_path, db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                              SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        tb_log_error("failed to open database %s: %s",
                     db_path, *db ? sqlite3_errmsg(*db) : "allocation failed");
        if (*db) { sqlite3_close(*db); *db = NULL; }
        return -1;
    }

    /* Set busy timeout for concurrent access (5 seconds) */
    sqlite3_busy_timeout(*db, 5000);

    /* Enable WAL mode for better concurrent read performance */
    char *err_msg = NULL;
    rc = sqlite3_exec(*db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        tb_log_warn("failed to enable WAL mode: %s", err_msg);
        sqlite3_free(err_msg);
    }

    /* Create schema */
    rc = sqlite3_exec(*db, SCHEMA_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        tb_log_error("failed to create schema: %s", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(*db);
        *db = NULL;
        return -1;
    }

    /* Restrict database file permissions to owner only */
    chmod(db_path, 0600);

    tb_log_info("database opened: %s", db_path);
    return 0;
}

void tb_db_close(sqlite3 *db) {
    if (db) {
        sqlite3_close(db);
        tb_log_info("database closed");
    }
}
