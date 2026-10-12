#ifndef SENSOR_TYPES_H
#define SENSOR_TYPES_H

#include <stdint.h>
#include <time.h>

typedef uint16_t sensor_id_t;

typedef enum {
    SENSOR_TEMPERATURE = 1,
    SENSOR_HUMIDITY    = 2,
    SENSOR_MOTION      = 3,
    SENSOR_DOOR        = 4,
    SENSOR_SMOKE_GAS   = 5
} sensor_type_t;

typedef struct {
    sensor_id_t id;
    sensor_type_t type;
    double value;
    time_t ts;
} sensor_data_t;

#endif