#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#include "gateway.h"
#include "logger.h"

#include "connection_manager.h"
#include "data_manager.h"
#include "storage_manager.h"


static volatile sig_atomic_t stop_requested = 0;


typedef enum {
    SHUTDOWN_SIGNAL = 0,
    SHUTDOWN_WORKER_FAILURE,
    SHUTDOWN_THREAD_CREATE_FAILURE
} shutdown_reason_t;


static void handle_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}


static int gateway_stop_requested(gateway_t *gateway)
{
    int stop;

    pthread_mutex_lock(&gateway->stop_mutex);
    stop = gateway->stop;
    pthread_mutex_unlock(&gateway->stop_mutex);

    return stop;
}


static void request_gateway_stop(gateway_t *gateway)
{
    pthread_mutex_lock(&gateway->stop_mutex);
    gateway->stop = 1;
    pthread_mutex_unlock(&gateway->stop_mutex);
}


static int parse_port(const char *text, uint16_t *port)
{
    char *end;
    long value;

    errno = 0;

    value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value < 1 ||
        value > 65535) {

        return -1;
    }

    *port = (uint16_t)value;

    return 0;
}


int main(int argc, char *argv[])
{
    gateway_t gateway = {0};

    pthread_t conn_thread;
    pthread_t data_thread;
    pthread_t storage_thread;

    int conn_started = 0;
    int data_started = 0;
    int storage_started = 0;

    int exit_code = 0;

    shutdown_reason_t shutdown_reason =
        SHUTDOWN_SIGNAL;


    /* =========================
       1. Đọc command-line
       ========================= */

    if (argc != 3) {
        fprintf(
            stderr,
            "Usage: %s <base_port> <http_port>\n",
            argv[0]
        );

        return 1;
    }

    if (parse_port(argv[1], &gateway.base_port) != 0 ||
        parse_port(argv[2], &gateway.http_port) != 0) {

        fprintf(stderr, "Invalid port\n");
        return 1;
    }

#ifdef DEBUG
    printf(
        "[DEBUG] Base TCP port: %u\n",
        gateway.base_port
    );

    printf(
        "[DEBUG] HTTP port: %u\n",
        gateway.http_port
    );
#endif


    /* =========================
       2. Khởi tạo stop mutex
       ========================= */

    gateway.stop = 0;

    if (pthread_mutex_init(
            &gateway.stop_mutex,
            NULL
        ) != 0) {

        fprintf(
            stderr,
            "Failed to initialize stop mutex\n"
        );

        return 1;
    }


    /* =========================
       3. Khởi tạo sbuffer
       ========================= */

    if (sbuffer_init(&gateway.sbuffer) != 0) {

        fprintf(
            stderr,
            "Failed to initialize sbuffer\n"
        );

        pthread_mutex_destroy(
            &gateway.stop_mutex
        );

        return 1;
    }


    /* =========================
       4. Signal
       ========================= */

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);


    /* =========================
       5. Tạo FIFO logger
       ========================= */

    if (logger_fifo_create() != 0) {

        sbuffer_destroy(
            &gateway.sbuffer
        );

        pthread_mutex_destroy(
            &gateway.stop_mutex
        );

        return 1;
    }


    /* =========================
       6. Fork logger process
       ========================= */

    gateway.log_pid = fork();

    if (gateway.log_pid < 0) {

        perror("fork");

        logger_fifo_remove();

        sbuffer_destroy(
            &gateway.sbuffer
        );

        pthread_mutex_destroy(
            &gateway.stop_mutex
        );

        return 1;
    }


    if (gateway.log_pid == 0) {

        logger_run();

        _exit(EXIT_SUCCESS);
    }


    if (logger_writer_open() != 0) {

        kill(
            gateway.log_pid,
            SIGKILL
        );

        waitpid(
            gateway.log_pid,
            NULL,
            0
        );

        logger_fifo_remove();

        sbuffer_destroy(
            &gateway.sbuffer
        );

        pthread_mutex_destroy(
            &gateway.stop_mutex
        );

        return 1;
    }


    /* =========================
       7. Log gateway startup
       ========================= */

    char message[200];

    snprintf(
        message,
        sizeof(message),
        "Gateway started (TCP base port %u, HTTP port %u)",
        gateway.base_port,
        gateway.http_port
    );

    logger_send(
        "MAIN",
        message
    );

#ifdef DEBUG
    printf("[MAIN] Gateway started\n");
#endif


    /* =========================
       8. Tạo 3 worker thread
       ========================= */

    if (pthread_create(
            &conn_thread,
            NULL,
            connection_manager_thread,
            &gateway
        ) != 0) {

        fprintf(
            stderr,
            "Failed to create Connection Manager thread\n"
        );

        exit_code = 1;
        shutdown_reason =
            SHUTDOWN_THREAD_CREATE_FAILURE;

        goto shutdown;
    }

    conn_started = 1;


    if (pthread_create(
            &data_thread,
            NULL,
            data_manager_thread,
            &gateway
        ) != 0) {

        fprintf(
            stderr,
            "Failed to create Data Manager thread\n"
        );

        exit_code = 1;
        shutdown_reason =
            SHUTDOWN_THREAD_CREATE_FAILURE;

        goto shutdown;
    }

    data_started = 1;


    if (pthread_create(
            &storage_thread,
            NULL,
            storage_manager_thread,
            &gateway
        ) != 0) {

        fprintf(
            stderr,
            "Failed to create Storage Manager thread\n"
        );

        exit_code = 1;
        shutdown_reason =
            SHUTDOWN_THREAD_CREATE_FAILURE;

        goto shutdown;
    }

    storage_started = 1;


    /* =========================
       9. Main chờ shutdown
       ========================= */

    while (1) {

        if (stop_requested) {

            shutdown_reason =
                SHUTDOWN_SIGNAL;

            break;
        }

        if (gateway_stop_requested(&gateway)) {

            shutdown_reason =
                SHUTDOWN_WORKER_FAILURE;

            exit_code = 1;

            break;
        }

        sleep(1);
    }


    /* =========================
       10. Controlled shutdown
       ========================= */

shutdown:

    request_gateway_stop(
        &gateway
    );

    sbuffer_shutdown(
        &gateway.sbuffer
    );


    /* =========================
       11. Join các thread đã tạo
       ========================= */

    if (conn_started) {
        pthread_join(
            conn_thread,
            NULL
        );
    }

    if (data_started) {
        pthread_join(
            data_thread,
            NULL
        );
    }

    if (storage_started) {
        pthread_join(
            storage_thread,
            NULL
        );
    }


    /* =========================
       12. Log shutdown
       ========================= */

    switch (shutdown_reason) {

        case SHUTDOWN_SIGNAL:
            logger_send(
                "MAIN",
                "Gateway shutting down (signal)"
            );
            break;

        case SHUTDOWN_WORKER_FAILURE:
            logger_send(
                "MAIN",
                "Gateway shutting down (worker failure)"
            );
            break;

        case SHUTDOWN_THREAD_CREATE_FAILURE:
            logger_send(
                "MAIN",
                "Gateway shutting down (thread creation failure)"
            );
            break;
    }


    logger_writer_close();


    /* =========================
       13. Chờ logger process
       ========================= */

    waitpid(
        gateway.log_pid,
        NULL,
        0
    );


    /* =========================
       14. Cleanup
       ========================= */

    sbuffer_destroy(
        &gateway.sbuffer
    );

    pthread_mutex_destroy(
        &gateway.stop_mutex
    );

    logger_fifo_remove();

#ifdef DEBUG
    printf("[MAIN] Gateway shutting down\n");
#endif

    return exit_code;
}
