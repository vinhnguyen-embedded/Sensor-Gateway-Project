#ifndef LOGGER_H
#define LOGGER_H

#define LOG_FIFO "logFifo"
#define LOG_FILE "gateway.log"

int logger_fifo_create(void);

int logger_writer_open(void);
void logger_writer_close(void);

int logger_send(const char *source, const char *message);

void logger_run(void);

void logger_fifo_remove(void);

#endif