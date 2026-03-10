#ifndef TB_DB_H
#define TB_DB_H

#include <sqlite3.h>

/* Open database and create schema if needed */
int tb_db_open(sqlite3 **db, const char *db_path);

/* Close database */
void tb_db_close(sqlite3 *db);

#endif /* TB_DB_H */
