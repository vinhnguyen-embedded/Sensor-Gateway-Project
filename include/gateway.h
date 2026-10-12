#ifndef GATEWAY_H
#define GATEWAY_H

#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include "sbuffer.h"
typedef struct {

    uint16_t base_port;
    uint16_t http_port;

    volatile sig_atomic_t stop;
    pthread_mutex_t stop_mutex;
    pid_t log_pid;
    sbuffer_t sbuffer;
    
} gateway_t;

#endif