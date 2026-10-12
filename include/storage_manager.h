#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <sqlite3.h>

typedef enum {
    SQL_CONNECTED,
    SQL_LOST,
    SQL_DOWN
} sql_state_t;

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *insert;
    sql_state_t state;
    int attempts;
    const char *db_path;
} storagemgr_t;

void *storage_manager_thread(void *arg);

#endif