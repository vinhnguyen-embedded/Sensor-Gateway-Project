#ifndef PROTOCOL_ADAPTER_H
#define PROTOCOL_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "sensor_types.h"


typedef enum {

    PARSE_ERROR = -1,

    PARSE_NEED_MORE = 0,

    PARSE_OK = 1

} parse_result_t;


typedef struct {

    const char *name;

    uint16_t port_offset;


    parse_result_t (*parse)(
        const uint8_t *buf,
        size_t len,
        sensor_data_t *out,
        size_t *consumed
    );


    size_t (*build_reply)(
        parse_result_t result,
        uint8_t *reply,
        size_t cap
    );

} protocol_adapter_t;


/*
 * Các adapter được hiện thực
 * trong protocol_adapter.c
 */
extern const protocol_adapter_t TBP_ADAPTER;
extern const protocol_adapter_t HLP_ADAPTER;
extern const protocol_adapter_t EVP_ADAPTER;
extern const protocol_adapter_t SGP_ADAPTER;


#endif