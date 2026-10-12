#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>

#include <sqlite3.h>

#include "storage_manager.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"


#define DATABASE_PATH "gateway.db"
#define RETRY_DELAY_SECONDS 1
#define MAX_SQL_ATTEMPTS 3


/*
 * ============================================================
 * Kiểm tra gateway có được yêu cầu dừng hay chưa
 * ============================================================
 */
static bool gateway_should_stop(
    gateway_t *gateway
)
{
    bool stop;

    pthread_mutex_lock(
        &gateway->stop_mutex
    );

    stop = gateway->stop;

    pthread_mutex_unlock(
        &gateway->stop_mutex
    );

    return stop;
}


/*
 * ============================================================
 * Yêu cầu main process shutdown gateway
 * ============================================================
 */
static void request_gateway_stop(
    gateway_t *gateway
)
{
    pthread_mutex_lock(
        &gateway->stop_mutex
    );

    gateway->stop = 1;

    pthread_mutex_unlock(
        &gateway->stop_mutex
    );
}


/*
 * ============================================================
 * Kiểm tra bảng SensorData đã tồn tại chưa
 *
 * return:
 *      1 = tồn tại
 *      0 = chưa tồn tại
 *     -1 = lỗi
 * ============================================================
 */
static int table_exists(
    sqlite3 *db
)
{
    sqlite3_stmt *statement = NULL;

    const char *sql =
        "SELECT name "
        "FROM sqlite_master "
        "WHERE type='table' "
        "AND name='SensorData';";


    if (
        sqlite3_prepare_v2(
            db,
            sql,
            -1,
            &statement,
            NULL
        ) != SQLITE_OK
    ) {
        return -1;
    }


    int result =
        sqlite3_step(
            statement
        );


    int exists;


    if (result == SQLITE_ROW) {

        exists = 1;

    } else if (result == SQLITE_DONE) {

        exists = 0;

    } else {

        exists = -1;
    }


    sqlite3_finalize(
        statement
    );


    return exists;
}


/*
 * ============================================================
 * Mở SQLite
 * Tạo bảng SensorData nếu chưa tồn tại
 * Prepare câu lệnh INSERT
 * ============================================================
 */
static int storage_connect(
    storagemgr_t *manager
)
{
    /*
     * Mở database.
     *
     * Nếu gateway.db chưa tồn tại,
     * SQLite sẽ tự tạo file.
     */
    if (
        sqlite3_open(
            manager->db_path,
            &manager->db
        ) != SQLITE_OK
    ) {

        if (manager->db != NULL) {

            sqlite3_close(
                manager->db
            );

            manager->db = NULL;
        }

        return -1;
    }


    /*
     * Nếu database đang BUSY,
     * SQLite chờ tối đa 1 giây.
     */
    sqlite3_busy_timeout(
        manager->db,
        1000
    );


    /*
     * Kiểm tra SensorData đã tồn tại chưa.
     */
    int existed =
        table_exists(
            manager->db
        );


    if (existed < 0) {

        sqlite3_close(
            manager->db
        );

        manager->db = NULL;

        return -1;
    }


    /*
     * Tạo bảng nếu chưa có.
     */
    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS SensorData ("
        "id INTEGER NOT NULL,"
        "type INTEGER NOT NULL,"
        "value REAL NOT NULL,"
        "ts INTEGER NOT NULL"
        ");";


    if (
        sqlite3_exec(
            manager->db,
            create_sql,
            NULL,
            NULL,
            NULL
        ) != SQLITE_OK
    ) {

        sqlite3_close(
            manager->db
        );

        manager->db = NULL;

        return -1;
    }


    /*
     * Chỉ log nếu bảng thực sự vừa được tạo.
     */
    if (!existed) {

        logger_send(
            "STOR",
            "New table SensorData created."
        );
    }


    /*
     * Chuẩn bị câu INSERT.
     *
     * Sau này chỉ cần bind dữ liệu vào
     * ?, ?, ?, ?
     */
    const char *insert_sql =
        "INSERT INTO SensorData "
        "(id, type, value, ts) "
        "VALUES (?, ?, ?, ?);";


    if (
        sqlite3_prepare_v2(
            manager->db,
            insert_sql,
            -1,
            &manager->insert,
            NULL
        ) != SQLITE_OK
    ) {

        sqlite3_close(
            manager->db
        );

        manager->db = NULL;

        return -1;
    }


    manager->state =
        SQL_CONNECTED;


    logger_send(
        "STOR",
        "Connection to SQL server established."
    );


#ifdef DEBUG

    printf(
        "[STOR] SQLite connected: %s\n",
        manager->db_path
    );

#endif


    return 0;
}


/*
 * ============================================================
 * Đóng SQLite
 * ============================================================
 */
static void storage_disconnect(
    storagemgr_t *manager
)
{
    if (
        manager->insert != NULL
    ) {

        sqlite3_finalize(
            manager->insert
        );

        manager->insert = NULL;
    }


    if (
        manager->db != NULL
    ) {

        sqlite3_close(
            manager->db
        );

        manager->db = NULL;
    }
}


/*
 * ============================================================
 * INSERT đúng một sensor_data_t
 *
 * Trả về mã kết quả của SQLite.
 * SQLITE_DONE = thành công
 * SQLITE_BUSY / SQLITE_LOCKED = DB đang bị khóa
 * ============================================================
 */
static int storage_insert(
    storagemgr_t *manager,
    const sensor_data_t *data
)
{
    if (
        manager->insert == NULL
    ) {
        return SQLITE_MISUSE;
    }


    sqlite3_reset(
        manager->insert
    );


    sqlite3_clear_bindings(
        manager->insert
    );


    /*
     * id
     */
    sqlite3_bind_int(
        manager->insert,
        1,
        data->id
    );


    /*
     * type
     */
    sqlite3_bind_int(
        manager->insert,
        2,
        data->type
    );


    /*
     * value
     */
    sqlite3_bind_double(
        manager->insert,
        3,
        data->value
    );


    /*
     * timestamp
     */
    sqlite3_bind_int64(
        manager->insert,
        4,
        (sqlite3_int64)data->ts
    );


    return sqlite3_step(
        manager->insert
    );
}


/*
 * ============================================================
 * STORAGE MANAGER THREAD
 * ============================================================
 */
void *storage_manager_thread(
    void *arg
)
{
    gateway_t *gateway =
        (gateway_t *)arg;


    storagemgr_t manager = {
        .db = NULL,
        .insert = NULL,
        .state = SQL_DOWN,
        .attempts = 0,
        .db_path = DATABASE_PATH
    };


    logger_send(
        "STOR",
        "Storage manager thread started"
    );


#ifdef DEBUG

    printf(
        "[STOR] Storage manager thread started\n"
    );

#endif


    /*
     * ========================================================
     * 1. KẾT NỐI DATABASE BAN ĐẦU
     * ========================================================
     */

    while (
        storage_connect(
            &manager
        ) != 0
    ) {

        manager.attempts++;


#ifdef DEBUG

        printf(
            "[STOR] SQL connect attempt %d/%d failed\n",
            manager.attempts,
            MAX_SQL_ATTEMPTS
        );

#endif


        /*
         * Đã thất bại 3 lần.
         */
        if (
            manager.attempts >=
            MAX_SQL_ATTEMPTS
        ) {

            manager.state =
                SQL_DOWN;


            logger_send(
                "STOR",
                "Unable to connect to SQL server."
            );


            request_gateway_stop(
                gateway
            );


            goto shutdown;
        }


        sleep(
            RETRY_DELAY_SECONDS
        );
    }


    /*
     * Connect thành công.
     */
    manager.attempts = 0;


    /*
     * ========================================================
     * 2. ĐỌC SBUFFER VÀ GHI SQLITE
     * ========================================================
     */

    while (
        !gateway_should_stop(
            gateway
        )
    ) {

        sensor_data_t data;


        /*
         * Chỉ lấy dữ liệu ra xem.
         *
         * CHƯA đánh dấu Storage đã xử lý.
         */
        int ret =
            sbuffer_peek_storage(
                &gateway->sbuffer,
                &data
            );


        if (ret != 0) {

            break;
        }


        /*
         * Measurement hiện tại chưa được commit.
         *
         * Nếu DB lỗi, ta vẫn giữ chính
         * measurement này để thử lại.
         */
        manager.attempts = 0;


        while (1) {

            int sql_result =
                storage_insert(
                    &manager,
                    &data
                );


            /*
             * =================================================
             * INSERT THÀNH CÔNG
             * =================================================
             */
            if (
                sql_result ==
                SQLITE_DONE
            ) {

#ifdef DEBUG

                printf(
                    "[STOR] saved id=%u type=%d value=%.2f ts=%ld\n",
                    data.id,
                    data.type,
                    data.value,
                    (long)data.ts
                );

#endif


                /*
                 * Chỉ bây giờ mới báo sbuffer rằng
                 * Storage Manager đã xử lý xong.
                 */
                sbuffer_commit_storage(
                    &gateway->sbuffer
                );


                /*
                 * Nếu trước đó DB từng bị lỗi nhưng
                 * giờ ghi lại thành công.
                 */
                if (
                    manager.state !=
                    SQL_CONNECTED
                ) {

                    manager.state =
                        SQL_CONNECTED;


                    logger_send(
                        "STOR",
                        "Connection to SQL server established."
                    );
                }


                manager.attempts = 0;


                break;
            }


            /*
             * =================================================
             * INSERT THẤT BẠI
             * =================================================
             */


            /*
             * Chỉ log "lost" một lần khi
             * chuyển CONNECTED -> LOST.
             */
            if (
                manager.state ==
                SQL_CONNECTED
            ) {

                logger_send(
                    "STOR",
                    "Connection to SQL server lost."
                );


                manager.state =
                    SQL_LOST;
            }


            /*
             * Đếm số lần ghi thất bại.
             */
            manager.attempts++;


#ifdef DEBUG

            printf(
                "[STOR] SQL retry %d/%d "
                "(SQLite error %d: %s)\n",
                manager.attempts,
                MAX_SQL_ATTEMPTS,
                sql_result,
                sqlite3_errmsg(manager.db)
            );

#endif


            /*
             * =================================================
             * THẤT BẠI ĐỦ 3 LẦN
             * =================================================
             */
            if (
                manager.attempts >=
                MAX_SQL_ATTEMPTS
            ) {

                manager.state =
                    SQL_DOWN;


                logger_send(
                    "STOR",
                    "Unable to connect to SQL server."
                );


                /*
                 * Báo main process shutdown gateway.
                 */
                request_gateway_stop(
                    gateway
                );


                /*
                 * Không commit sbuffer.
                 *
                 * Measurement vẫn chưa được đánh dấu
                 * là Storage đã xử lý.
                 */
                goto shutdown;
            }


            /*
             * Chờ rồi thử INSERT lại
             * chính measurement này.
             */
            sleep(
                RETRY_DELAY_SECONDS
            );
        }
    }


shutdown:

    /*
     * ========================================================
     * 3. CLEANUP
     * ========================================================
     */

    storage_disconnect(
        &manager
    );


#ifdef DEBUG

    printf(
        "[STOR] Storage manager thread stopped\n"
    );

#endif


    return NULL;
}