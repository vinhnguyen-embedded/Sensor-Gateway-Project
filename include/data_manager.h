#ifndef DATA_MANAGER_H
#define DATA_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "sensor_types.h"
#include "sensor_config.h"


typedef enum {
    STATE_COLLECTING,
    STATE_NORMAL,
    STATE_ALERT
} sensor_state_t;


typedef struct {
    uint16_t room;
    sensor_id_t id;
    sensor_type_t type;
} sensor_info_t;


typedef struct {
    sensor_info_t info;

    double window[RUN_AVG_LENGTH];
    size_t count;
    size_t next;
    double running_avg;

    sensor_state_t state;

    bool door_initialized;
    int last_door_state;

} datamgr_sensor_t;


typedef struct {
    datamgr_sensor_t sensors[MAX_SENSORS];
    size_t n;
} datamgr_t;


typedef void (*sensor_handler_t)(
    datamgr_t *,
    datamgr_sensor_t *,
    const sensor_data_t *
);


void *data_manager_thread(void *arg);


#endif