#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "data_manager.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"
#include "sensor_config.h"


#define ROOM_SENSOR_FILE "room_sensor.txt"


/*
 * ============================================================
 * Chuyển chuỗi type trong room_sensor.txt
 * thành sensor_type_t
 * ============================================================
 */

static int parse_sensor_type(
    const char *text,
    sensor_type_t *type
)
{
    if (strcmp(text, "temperature") == 0) {

        *type = SENSOR_TEMPERATURE;

        return 0;
    }


    if (strcmp(text, "humidity") == 0) {

        *type = SENSOR_HUMIDITY;

        return 0;
    }


    if (strcmp(text, "motion") == 0) {

        *type = SENSOR_MOTION;

        return 0;
    }


    if (strcmp(text, "door") == 0) {

        *type = SENSOR_DOOR;

        return 0;
    }


    if (strcmp(text, "smoke_gas") == 0) {

        *type = SENSOR_SMOKE_GAS;

        return 0;
    }


    return -1;
}


/*
 * ============================================================
 * Nạp room_sensor.txt
 * ============================================================
 */

static int load_room_sensor_file(
    datamgr_t *manager
)
{
    FILE *file =
        fopen(
            ROOM_SENSOR_FILE,
            "r"
        );


    if (file == NULL) {

        perror("fopen room_sensor");

        return -1;
    }


    char line[256];


    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {

        /*
         * Bỏ comment và dòng trống.
         */
        if (
            line[0] == '#' ||
            line[0] == '\n'
        ) {

            continue;
        }


        unsigned int room;

        unsigned int id;

        char type_text[32];


        if (
            sscanf(
                line,
                "%u %u %31s",
                &room,
                &id,
                type_text
            ) != 3
        ) {

            continue;
        }


        if (
            room > UINT16_MAX ||
            id > UINT16_MAX
        ) {

            continue;
        }


        sensor_type_t type;


        if (
            parse_sensor_type(
                type_text,
                &type
            ) != 0
        ) {

            continue;
        }


        if (
            manager->n >=
            MAX_SENSORS
        ) {

            break;
        }


        datamgr_sensor_t *sensor =
            &manager->sensors[
                manager->n
            ];


        memset(
            sensor,
            0,
            sizeof(*sensor)
        );


        sensor->info.room =
            (uint16_t)room;

        sensor->info.id =
            (sensor_id_t)id;

        sensor->info.type =
            type;


        sensor->state =
            STATE_COLLECTING;


        sensor->door_initialized =
            false;


        manager->n++;
    }


    fclose(file);


#ifdef DEBUG

    printf(
        "[DATA] Loaded %zu sensors from %s\n",
        manager->n,
        ROOM_SENSOR_FILE
    );

#endif


    return 0;
}


/*
 * ============================================================
 * Tìm sensor theo ID
 * ============================================================
 */

static datamgr_sensor_t *find_sensor(
    datamgr_t *manager,
    sensor_id_t id
)
{
    for (
        size_t i = 0;
        i < manager->n;
        i++
    ) {

        if (
            manager->sensors[i].
            info.id == id
        ) {

            return
                &manager->sensors[i];
        }
    }


    return NULL;
}


/*
 * ============================================================
 * Log sensor không hợp lệ
 * ============================================================
 */

static void log_invalid_sensor(
    sensor_id_t id
)
{
    char message[200];


    snprintf(
        message,
        sizeof(message),
        "Received sensor data with invalid sensor node ID %u",
        id
    );


    logger_send(
        "DATA",
        message
    );
}


/*
 * ============================================================
 * Running average
 *
 * Chỉ trả về true khi đã đủ 5 mẫu.
 * ============================================================
 */

static bool update_running_average(
    datamgr_sensor_t *sensor,
    double value
)
{
    sensor->window[
        sensor->next
    ] = value;


    sensor->next =
        (
            sensor->next + 1
        ) %
        RUN_AVG_LENGTH;


    if (
        sensor->count <
        RUN_AVG_LENGTH
    ) {

        sensor->count++;
    }


    /*
     * Chưa đủ 5 mẫu.
     */
    if (
        sensor->count <
        RUN_AVG_LENGTH
    ) {

        sensor->state =
            STATE_COLLECTING;

        return false;
    }


    double sum = 0.0;


    for (
        size_t i = 0;
        i < RUN_AVG_LENGTH;
        i++
    ) {

        sum +=
            sensor->window[i];
    }


    sensor->running_avg =
        sum /
        RUN_AVG_LENGTH;


    return true;
}


/*
 * ============================================================
 * Temperature handler
 * ============================================================
 */

static void handle_temperature(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;


    if (
        !update_running_average(
            sensor,
            data->value
        )
    ) {

        return;
    }


    double avg =
        sensor->running_avg;


#ifdef DEBUG

    printf(
        "[DATA] temp id=%u avg=%.2f\n",
        data->id,
        avg
    );

#endif


    if (avg < TEMP_MIN_C) {

        /*
         * Chỉ log lúc mới chuyển
         * NORMAL/COLLECTING -> ALERT.
         */
        if (
            sensor->state !=
            STATE_ALERT
        ) {

            char message[200];


            snprintf(
                message,
                sizeof(message),
                "The sensor node with %u reports it's too cold (running avg temperature = %.1f)",
                data->id,
                avg
            );


            logger_send(
                "DATA",
                message
            );
        }


        sensor->state =
            STATE_ALERT;
    }

    else if (
        avg > TEMP_MAX_C
    ) {

        if (
            sensor->state !=
            STATE_ALERT
        ) {

            char message[200];


            snprintf(
                message,
                sizeof(message),
                "The sensor node with %u reports it's too hot (running avg temperature = %.1f)",
                data->id,
                avg
            );


            logger_send(
                "DATA",
                message
            );
        }


        sensor->state =
            STATE_ALERT;
    }

    else {

        sensor->state =
            STATE_NORMAL;
    }
}


/*
 * ============================================================
 * Humidity handler
 * ============================================================
 */

static void handle_humidity(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;


    if (
        !update_running_average(
            sensor,
            data->value
        )
    ) {

        return;
    }


    double avg =
        sensor->running_avg;


#ifdef DEBUG

    printf(
        "[DATA] humidity id=%u avg=%.2f\n",
        data->id,
        avg
    );

#endif


    if (
        avg <
        HUMIDITY_MIN
    ) {

        if (
            sensor->state !=
            STATE_ALERT
        ) {

            char message[200];


            snprintf(
                message,
                sizeof(message),
                "The sensor node with %u reports it's too dry (running avg humidity = %.1f)",
                data->id,
                avg
            );


            logger_send(
                "DATA",
                message
            );
        }


        sensor->state =
            STATE_ALERT;
    }

    else if (
        avg >
        HUMIDITY_MAX
    ) {

        if (
            sensor->state !=
            STATE_ALERT
        ) {

            char message[200];


            snprintf(
                message,
                sizeof(message),
                "The sensor node with %u reports it's too humid (running avg humidity = %.1f)",
                data->id,
                avg
            );


            logger_send(
                "DATA",
                message
            );
        }


        sensor->state =
            STATE_ALERT;
    }

    else {

        sensor->state =
            STATE_NORMAL;
    }
}


/*
 * ============================================================
 * Motion handler
 *
 * Motion là event, không tính average.
 * ============================================================
 */

static void handle_motion(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;
    (void)sensor;


    char message[200];


    snprintf(
        message,
        sizeof(message),
        "The sensor node with %u detected motion",
        data->id
    );


    logger_send(
        "DATA",
        message
    );
}


/*
 * ============================================================
 * Door handler
 *
 * Chỉ log khi trạng thái thay đổi.
 * ============================================================
 */

static void handle_door(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;


    int state =
        data->value >= 0.5
        ? 1
        : 0;


    /*
     * Nếu trạng thái giống lần trước
     * thì không log lại.
     */
    if (
        sensor->door_initialized &&
        sensor->last_door_state ==
        state
    ) {

        return;
    }


    sensor->door_initialized =
        true;


    sensor->last_door_state =
        state;


    char message[200];


    snprintf(
        message,
        sizeof(message),

        "The sensor node with %u reports the door is %s",

        data->id,

        state
            ? "open"
            : "closed"
    );


    logger_send(
        "DATA",
        message
    );
}


/*
 * ============================================================
 * Smoke/Gas handler
 *
 * Không average.
 * So ngưỡng ngay.
 * ============================================================
 */

static void handle_smoke_gas(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;


    if (
        data->value >=
        GAS_ALARM_PPM
    ) {

        if (
            sensor->state !=
            STATE_ALERT
        ) {

            char message[200];


            snprintf(
                message,
                sizeof(message),

                "The sensor node with %u reports a smoke/gas alarm (concentration = %.1f ppm)",

                data->id,
                data->value
            );


            logger_send(
                "DATA",
                message
            );
        }


        sensor->state =
            STATE_ALERT;

    } else {

        sensor->state =
            STATE_NORMAL;
    }
}


/*
 * ============================================================
 * Handler table
 *
 * sensor_type_t được dùng làm index.
 * ============================================================
 */

static sensor_handler_t HANDLERS[] = {

    [SENSOR_TEMPERATURE] =
        handle_temperature,

    [SENSOR_HUMIDITY] =
        handle_humidity,

    [SENSOR_MOTION] =
        handle_motion,

    [SENSOR_DOOR] =
        handle_door,

    [SENSOR_SMOKE_GAS] =
        handle_smoke_gas
};


/*
 * ============================================================
 * DATA MANAGER THREAD
 * ============================================================
 */

void *data_manager_thread(
    void *arg
)
{
    gateway_t *gateway =
        (gateway_t *)arg;


    logger_send(
        "DATA",
        "Data manager thread started"
    );


#ifdef DEBUG

    printf(
        "[DATA] Data manager thread started\n"
    );

#endif


    /*
     * State của toàn bộ sensor chỉ thuộc
     * Data Manager thread này.
     *
     * Vì vậy không cần mutex riêng.
     */
    datamgr_t manager;


    memset(
        &manager,
        0,
        sizeof(manager)
    );


    if (
        load_room_sensor_file(
            &manager
        ) != 0
    ) {

        logger_send(
            "DATA",
            "Unable to load room-sensor file"
        );
    }


    sensor_data_t data;


    while (1) {

        int result =
            sbuffer_read_data(
                &gateway->sbuffer,
                &data
            );


        if (result != 0) {

            break;
        }


#ifdef DEBUG

        printf(
            "[DATA] id=%u type=%d value=%.2f ts=%ld\n",
            data.id,
            data.type,
            data.value,
            (long)data.ts
        );

#endif


        /*
         * ====================================================
         * 1. Tìm ID trong room_sensor
         * ====================================================
         */

        datamgr_sensor_t *sensor =
            find_sensor(
                &manager,
                data.id
            );


        if (sensor == NULL) {

            log_invalid_sensor(
                data.id
            );

            continue;
        }


        /*
         * ====================================================
         * 2. ID đúng nhưng loại sensor sai
         * ====================================================
         */

        if (
            sensor->info.type !=
            data.type
        ) {

            log_invalid_sensor(
                data.id
            );

            continue;
        }


        /*
         * ====================================================
         * 3. Chọn handler theo sensor type
         * ====================================================
         */

        if (
            data.type <
            SENSOR_TEMPERATURE ||

            data.type >
            SENSOR_SMOKE_GAS ||

            HANDLERS[data.type] ==
            NULL
        ) {

            log_invalid_sensor(
                data.id
            );

            continue;
        }


        HANDLERS[
            data.type
        ](
            &manager,
            sensor,
            &data
        );
    }


#ifdef DEBUG

    printf(
        "[DATA] Data manager thread stopped\n"
    );

#endif


    return NULL;
}