#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

#include "protocol_adapter.h"

#define TBP_FRAME_SIZE 18

#define HLP_MAX_LINE 64

#define SGP_FRAME_SIZE 11

#define SGP_MAGIC 0x7E
#define SGP_ACK   0x06
#define SGP_NAK   0x15


/*
 * ============================================================
 * Helper: đọc integer big-endian
 * ============================================================
 */

static uint16_t read_be16(const uint8_t *p)
{
    return
        ((uint16_t)p[0] << 8) |
        (uint16_t)p[1];
}


static uint32_t read_be32(const uint8_t *p)
{
    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        (uint32_t)p[3];
}


/*
 * ============================================================
 * Helper: đọc uint64 little-endian
 * ============================================================
 */

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t value = 0;

    for (int i = 7; i >= 0; i--) {

        value <<= 8;

        value |= p[i];
    }

    return value;
}


/*
 * ============================================================
 * CRC-16/CCITT-FALSE
 *
 * polynomial = 0x1021
 * init       = 0xFFFF
 * ============================================================
 */

static uint16_t crc16_ccitt_false(
    const uint8_t *data,
    size_t len
)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {

        crc ^=
            (uint16_t)data[i] << 8;

        for (int bit = 0; bit < 8; bit++) {

            if (crc & 0x8000) {

                crc =
                    (uint16_t)(
                        (crc << 1) ^ 0x1021
                    );

            } else {

                crc =
                    (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}


/*
 * Các protocol không có reply.
 */
static size_t no_reply(
    parse_result_t result,
    uint8_t *reply,
    size_t cap
)
{
    (void)result;
    (void)reply;
    (void)cap;

    return 0;
}


/*
 * ============================================================
 * TBP
 *
 * 18 bytes
 *
 * id    : uint16 little-endian
 * value : double little-endian
 * ts    : int64 little-endian
 * ============================================================
 */

static parse_result_t parse_tbp(
    const uint8_t *buf,
    size_t len,
    sensor_data_t *out,
    size_t *consumed
)
{
    *consumed = 0;


    if (len < TBP_FRAME_SIZE) {

        return PARSE_NEED_MORE;
    }


    out->id =
        (uint16_t)buf[0] |
        ((uint16_t)buf[1] << 8);


    uint64_t value_bits =
        read_le64(buf + 2);

    memcpy(
        &out->value,
        &value_bits,
        sizeof(out->value)
    );


    uint64_t ts_bits =
        read_le64(buf + 10);

    int64_t ts;

    memcpy(
        &ts,
        &ts_bits,
        sizeof(ts)
    );


    out->type =
        SENSOR_TEMPERATURE;

    out->ts =
        (time_t)ts;


    *consumed =
        TBP_FRAME_SIZE;


    /*
     * Dải hợp lệ temperature:
     * -40 ... 85 °C
     */
    if (
        !isfinite(out->value) ||
        out->value < -40.0 ||
        out->value > 85.0
    ) {

        return PARSE_ERROR;
    }


    return PARSE_OK;
}


/*
 * ============================================================
 * HLP
 *
 * HUM <id> <value> <ts>\n
 *
 * tối đa 64 byte kể cả LF.
 * ============================================================
 */

static parse_result_t parse_hlp(
    const uint8_t *buf,
    size_t len,
    sensor_data_t *out,
    size_t *consumed
)
{
    *consumed = 0;


    /*
     * Tìm ký tự LF.
     */
    const uint8_t *lf =
        memchr(
            buf,
            '\n',
            len
        );


    /*
     * Chưa có LF.
     */
    if (lf == NULL) {

        if (len < HLP_MAX_LINE) {

            return PARSE_NEED_MORE;
        }


        /*
         * Dòng quá dài.
         */
        *consumed = 1;

        return PARSE_ERROR;
    }


    size_t line_len =
        (size_t)(lf - buf) + 1;


    if (line_len > HLP_MAX_LINE) {

        *consumed = line_len;

        return PARSE_ERROR;
    }


    char line[HLP_MAX_LINE + 1];


    memcpy(
        line,
        buf,
        line_len - 1
    );


    line[line_len - 1] = '\0';


    *consumed = line_len;


    /*
     * Bắt buộc bắt đầu bằng:
     *
     * HUM
     */
    if (
        strncmp(
            line,
            "HUM ",
            4
        ) != 0
    ) {

        return PARSE_ERROR;
    }


    /*
     * Theo đề các field cách nhau
     * đúng một dấu space.
     */
    int spaces = 0;


    for (
        size_t i = 0;
        i < line_len - 1;
        i++
    ) {

        if (line[i] == ' ') {

            spaces++;

        } else if (
            line[i] == '\t' ||
            line[i] == '\r'
        ) {

            return PARSE_ERROR;
        }
    }


    if (spaces != 3) {

        return PARSE_ERROR;
    }


    unsigned long id;

    double value;

    long long ts;

    char extra;


    int fields =
        sscanf(
            line,
            "HUM %lu %lf %lld %c",
            &id,
            &value,
            &ts,
            &extra
        );


    if (
        fields != 3 ||
        id > UINT16_MAX ||
        !isfinite(value) ||
        value < 0.0 ||
        value > 100.0
    ) {

        return PARSE_ERROR;
    }


    out->id =
        (sensor_id_t)id;

    out->type =
        SENSOR_HUMIDITY;

    out->value =
        value;

    out->ts =
        (time_t)ts;


    return PARSE_OK;
}


/*
 * ============================================================
 * EVP
 *
 * type | length | payload
 *
 * Motion:
 * 01 06 | id(2) | ts(4)
 *
 * Door:
 * 02 07 | id(2) | state(1) | ts(4)
 * ============================================================
 */

static parse_result_t parse_evp(
    const uint8_t *buf,
    size_t len,
    sensor_data_t *out,
    size_t *consumed
)
{
    *consumed = 0;


    /*
     * Cần ít nhất type + length.
     */
    if (len < 2) {

        return PARSE_NEED_MORE;
    }


    uint8_t type =
        buf[0];

    uint8_t payload_len =
        buf[1];


    size_t frame_len =
        2 + (size_t)payload_len;


    /*
     * Type không hợp lệ.
     */
    if (
        type != 0x01 &&
        type != 0x02
    ) {

        *consumed = 1;

        return PARSE_ERROR;
    }


    uint8_t expected_len =
        (type == 0x01)
        ? 6
        : 7;


    /*
     * Length sai.
     */
    if (
        payload_len !=
        expected_len
    ) {

        *consumed = 1;

        return PARSE_ERROR;
    }


    if (len < frame_len) {

        return PARSE_NEED_MORE;
    }


    out->id =
        read_be16(buf + 2);


    /*
     * Motion
     */
    if (type == 0x01) {

        out->type =
            SENSOR_MOTION;

        out->value =
            1.0;

        out->ts =
            (time_t)read_be32(
                buf + 4
            );
    }


    /*
     * Door
     */
    else {

        uint8_t state =
            buf[4];


        if (state > 1) {

            *consumed =
                frame_len;

            return PARSE_ERROR;
        }


        out->type =
            SENSOR_DOOR;

        out->value =
            (double)state;

        out->ts =
            (time_t)read_be32(
                buf + 5
            );
    }


    *consumed =
        frame_len;


    return PARSE_OK;
}


/*
 * ============================================================
 * SGP
 *
 * 0      : magic 0x7E
 * 1..2   : id
 * 3..4   : ppm
 * 5..8   : timestamp
 * 9..10  : CRC16
 * ============================================================
 */

static parse_result_t parse_sgp(
    const uint8_t *buf,
    size_t len,
    sensor_data_t *out,
    size_t *consumed
)
{
    *consumed = 0;


    if (len == 0) {

        return PARSE_NEED_MORE;
    }


    /*
     * Sai magic.
     */
    if (buf[0] != SGP_MAGIC) {

        *consumed = 1;

        return PARSE_ERROR;
    }


    if (len < SGP_FRAME_SIZE) {

        return PARSE_NEED_MORE;
    }


    uint16_t received_crc =
        read_be16(buf + 9);


    /*
     * CRC chỉ tính byte offset 1..8.
     */
    uint16_t calculated_crc =
        crc16_ccitt_false(
            buf + 1,
            8
        );


    *consumed =
        SGP_FRAME_SIZE;


    if (
        received_crc !=
        calculated_crc
    ) {

        return PARSE_ERROR;
    }


    uint16_t ppm =
        read_be16(buf + 3);


    /*
     * Smoke/Gas hợp lệ:
     * 0 ... 10000 ppm
     */
    if (ppm > 10000) {

        return PARSE_ERROR;
    }


    out->id =
        read_be16(buf + 1);

    out->type =
        SENSOR_SMOKE_GAS;

    out->value =
        (double)ppm;

    out->ts =
        (time_t)read_be32(
            buf + 5
        );


    return PARSE_OK;
}


/*
 * ============================================================
 * SGP reply
 *
 * ACK = 0x06
 * NAK = 0x15
 * ============================================================
 */

static size_t build_sgp_reply(
    parse_result_t result,
    uint8_t *reply,
    size_t cap
)
{
    if (
        cap < 1 ||
        result == PARSE_NEED_MORE
    ) {

        return 0;
    }


    if (result == PARSE_OK) {

        reply[0] = SGP_ACK;

    } else {

        reply[0] = SGP_NAK;
    }


    return 1;
}


/*
 * ============================================================
 * Các protocol adapter
 * ============================================================
 */

const protocol_adapter_t TBP_ADAPTER = {
    "TBP",
    0,
    parse_tbp,
    no_reply
};


const protocol_adapter_t HLP_ADAPTER = {
    "HLP",
    1,
    parse_hlp,
    no_reply
};


const protocol_adapter_t EVP_ADAPTER = {
    "EVP",
    2,
    parse_evp,
    no_reply
};


const protocol_adapter_t SGP_ADAPTER = {
    "SGP",
    3,
    parse_sgp,
    build_sgp_reply
};