#include <stdlib.h>

#include "sbuffer.h"


int sbuffer_init(sbuffer_t *buffer)
{
    buffer->head = NULL;
    buffer->tail = NULL;
    buffer->size = 0;
    buffer->shutdown = false;

    if (pthread_mutex_init(&buffer->lock, NULL) != 0) {
        return -1;
    }

    if (pthread_cond_init(&buffer->not_empty, NULL) != 0) {

        pthread_mutex_destroy(&buffer->lock);

        return -1;
    }

    return 0;
}


int sbuffer_insert(
    sbuffer_t *buffer,
    const sensor_data_t *data
)
{
    sbuffer_node_t *node =
        malloc(sizeof(sbuffer_node_t));

    if (node == NULL) {
        return -1;
    }

    node->data = *data;

    node->read_by_data = false;
    node->read_by_storage = false;

    node->next = NULL;


    pthread_mutex_lock(&buffer->lock);


    if (buffer->shutdown) {

        pthread_mutex_unlock(&buffer->lock);

        free(node);

        return -1;
    }


    if (buffer->tail == NULL) {

        buffer->head = node;
        buffer->tail = node;

    } else {

        buffer->tail->next = node;
        buffer->tail = node;
    }


    buffer->size++;


    /*
     * Đánh thức Data Manager và Storage Manager
     * nếu chúng đang chờ dữ liệu.
     */
    pthread_cond_broadcast(&buffer->not_empty);


    pthread_mutex_unlock(&buffer->lock);


    return 0;
}
int sbuffer_read_data(
    sbuffer_t *buffer,
    sensor_data_t *data
)
{
    pthread_mutex_lock(&buffer->lock);

    while (1) {

        sbuffer_node_t *node = buffer->head;

        /* Tìm node mà Data Manager chưa đọc */
        while (node != NULL && node->read_by_data) {
            node = node->next;
        }

        if (node != NULL) {

            *data = node->data;
            node->read_by_data = true;

            /*
             * Storage đã đọc trước rồi
             * -> cả hai đã đọc -> xóa node.
             */
            if (node->read_by_storage) {

                sbuffer_node_t *prev = NULL;
                sbuffer_node_t *current = buffer->head;

                while (current != node) {
                    prev = current;
                    current = current->next;
                }

                if (prev == NULL) {
                    buffer->head = node->next;
                } else {
                    prev->next = node->next;
                }

                if (buffer->tail == node) {
                    buffer->tail = prev;
                }

                buffer->size--;

                free(node);
            }

            pthread_mutex_unlock(&buffer->lock);

            return 0;
        }

        /*
         * Không còn dữ liệu Data cần đọc
         * và buffer đã shutdown.
         */
        if (buffer->shutdown) {

            pthread_mutex_unlock(&buffer->lock);

            return 1;
        }

        /*
         * Không có dữ liệu -> ngủ.
         */
        pthread_cond_wait(
            &buffer->not_empty,
            &buffer->lock
        );
    }
}


int sbuffer_read_storage(
    sbuffer_t *buffer,
    sensor_data_t *data
)
{
    pthread_mutex_lock(&buffer->lock);

    while (1) {

        sbuffer_node_t *node = buffer->head;

        /* Tìm node mà Storage chưa đọc */
        while (node != NULL && node->read_by_storage) {
            node = node->next;
        }

        if (node != NULL) {

            *data = node->data;
            node->read_by_storage = true;

            /*
             * Data đã đọc trước rồi
             * -> cả hai đã đọc -> xóa node.
             */
            if (node->read_by_data) {

                sbuffer_node_t *prev = NULL;
                sbuffer_node_t *current = buffer->head;

                while (current != node) {
                    prev = current;
                    current = current->next;
                }

                if (prev == NULL) {
                    buffer->head = node->next;
                } else {
                    prev->next = node->next;
                }

                if (buffer->tail == node) {
                    buffer->tail = prev;
                }

                buffer->size--;

                free(node);
            }

            pthread_mutex_unlock(&buffer->lock);

            return 0;
        }

        if (buffer->shutdown) {

            pthread_mutex_unlock(&buffer->lock);

            return 1;
        }

        pthread_cond_wait(
            &buffer->not_empty,
            &buffer->lock
        );
    }
}


void sbuffer_shutdown(sbuffer_t *buffer)
{
    pthread_mutex_lock(&buffer->lock);

    buffer->shutdown = true;

    /*
     * Đánh thức Data và Storage
     * nếu đang ngủ trong cond_wait().
     */
    pthread_cond_broadcast(
        &buffer->not_empty
    );

    pthread_mutex_unlock(&buffer->lock);
}


void sbuffer_destroy(sbuffer_t *buffer)
{
    pthread_mutex_lock(&buffer->lock);

    sbuffer_node_t *node = buffer->head;

    while (node != NULL) {

        sbuffer_node_t *next = node->next;

        free(node);

        node = next;
    }

    buffer->head = NULL;
    buffer->tail = NULL;
    buffer->size = 0;

    pthread_mutex_unlock(&buffer->lock);

    pthread_cond_destroy(
        &buffer->not_empty
    );

    pthread_mutex_destroy(
        &buffer->lock
    );
}
int sbuffer_peek_storage(
    sbuffer_t *buffer,
    sensor_data_t *data
)
{
    pthread_mutex_lock(
        &buffer->lock
    );

    while (1) {

        sbuffer_node_t *node =
            buffer->head;

        while (
            node != NULL &&
            node->read_by_storage
        ) {
            node = node->next;
        }

        if (node != NULL) {

            *data = node->data;

            pthread_mutex_unlock(
                &buffer->lock
            );

            return 0;
        }

        if (buffer->shutdown) {

            pthread_mutex_unlock(
                &buffer->lock
            );

            return 1;
        }

        pthread_cond_wait(
            &buffer->not_empty,
            &buffer->lock
        );
    }
}


int sbuffer_commit_storage(
    sbuffer_t *buffer
)
{
    pthread_mutex_lock(
        &buffer->lock
    );

    sbuffer_node_t *previous = NULL;

    sbuffer_node_t *node =
        buffer->head;


    while (
        node != NULL &&
        node->read_by_storage
    ) {

        previous = node;
        node = node->next;
    }


    if (node == NULL) {

        pthread_mutex_unlock(
            &buffer->lock
        );

        return -1;
    }


    node->read_by_storage = true;


    /*
     * Data Manager cũng đã đọc
     * -> lúc này mới được xóa.
     */
    if (node->read_by_data) {

        if (previous == NULL) {

            buffer->head =
                node->next;

        } else {

            previous->next =
                node->next;
        }


        if (
            buffer->tail ==
            node
        ) {

            buffer->tail =
                previous;
        }


        free(node);

        buffer->size--;
    }


    pthread_mutex_unlock(
        &buffer->lock
    );


    return 0;
}