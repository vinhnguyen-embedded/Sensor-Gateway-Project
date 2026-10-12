#ifndef SBUFFER_H
#define SBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#include "sensor_types.h"


typedef struct sbuffer_node {
    sensor_data_t data;

    bool read_by_data;
    bool read_by_storage;

    struct sbuffer_node *next;

} sbuffer_node_t;


typedef struct {
    sbuffer_node_t *head;
    sbuffer_node_t *tail;

    size_t size;

    bool shutdown;

    pthread_mutex_t lock;
    pthread_cond_t not_empty;

} sbuffer_t;


/* Khởi tạo buffer */
int sbuffer_init(sbuffer_t *buffer);


/* Connection Manager thêm dữ liệu */
int sbuffer_insert(
    sbuffer_t *buffer,
    const sensor_data_t *data
);


/* Data Manager đọc */
int sbuffer_read_data(
    sbuffer_t *buffer,
    sensor_data_t *data
);


/* Storage Manager đọc */
int sbuffer_read_storage(
    sbuffer_t *buffer,
    sensor_data_t *data
);


/* Báo buffer shutdown */
void sbuffer_shutdown(sbuffer_t *buffer);


/* Giải phóng tài nguyên */
void sbuffer_destroy(sbuffer_t *buffer);

int sbuffer_peek_storage(
    sbuffer_t *buffer,
    sensor_data_t *data
);

int sbuffer_commit_storage(
    sbuffer_t *buffer
);
#endif