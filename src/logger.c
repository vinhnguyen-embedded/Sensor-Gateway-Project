#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "logger.h"


static int log_fd = -1;

static pthread_mutex_t log_mutex =
    PTHREAD_MUTEX_INITIALIZER;


int logger_fifo_create(void)
{
    if (mkfifo(LOG_FIFO, 0666) == -1) {

        if (errno != EEXIST) {
            perror("mkfifo");
            return -1;
        }
    }

    return 0;
}


int logger_writer_open(void)
{
    log_fd = open(LOG_FIFO, O_WRONLY);

    if (log_fd == -1) {
        perror("open logFifo writer");
        return -1;
    }

    return 0;
}


void logger_writer_close(void)
{
    if (log_fd != -1) {
        close(log_fd);
        log_fd = -1;
    }
}


int logger_send(const char *source, const char *message)
{
    char event[256];

    if (strlen(message) > 200) {
        return -1;
    }

    int len = snprintf(
        event,
        sizeof(event),
        "%s %s\n",
        source,
        message
    );

    if (len < 0 || len >= (int)sizeof(event)) {
        return -1;
    }

    pthread_mutex_lock(&log_mutex);

    ssize_t written;

    do {
        written = write(log_fd, event, len);
    } while (written == -1 && errno == EINTR);

    pthread_mutex_unlock(&log_mutex);

    if (written != len) {
        return -1;
    }

    return 0;
}


void logger_run(void)
{
    int fifo_fd = open(LOG_FIFO, O_RDONLY);

    if (fifo_fd == -1) {
        perror("open logFifo reader");
        return;
    }

    FILE *fifo_stream = fdopen(fifo_fd, "r");

    if (fifo_stream == NULL) {
        perror("fdopen");
        close(fifo_fd);
        return;
    }

    /*
     * Đề không quy định append hay truncate.
     * Dùng "w" để mỗi lần chạy có một log mới,
     * sequence bắt đầu lại từ 1.
     */
    FILE *log_file = fopen(LOG_FILE, "w");

    if (log_file == NULL) {
        perror("fopen gateway.log");
        fclose(fifo_stream);
        return;
    }

    char event[256];
    unsigned long sequence = 1;

    while (fgets(event, sizeof(event), fifo_stream) != NULL) {

        time_t now = time(NULL);

        struct tm local_time;

        localtime_r(
            &now,
            &local_time
        );

        char time_string[20];

        strftime(
            time_string,
            sizeof(time_string),
            "%Y-%m-%d %H:%M:%S",
            &local_time
        );

        fprintf(
            log_file,
            "%lu %s %s",
            sequence,
            time_string,
            event
        );

        fflush(log_file);

        sequence++;
    }

    fclose(log_file);
    fclose(fifo_stream);
}


void logger_fifo_remove(void)
{
    unlink(LOG_FIFO);
}