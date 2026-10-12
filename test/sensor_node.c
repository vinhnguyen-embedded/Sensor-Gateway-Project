#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>

#include "sensor_types.h"


#define TBP_FRAME_SIZE 18
#define HLP_MAX_FRAME  64
#define EVP_MAX_FRAME  9
#define SGP_FRAME_SIZE 11

#define MAX_FRAME_SIZE 128

#define SGP_MAGIC 0x7E
#define SGP_ACK   0x06
#define SGP_NAK   0x15

#define SGP_MAX_RETRIES 3


/*
 * ============================================================
 * Helper: endian
 * ============================================================
 */

static void write_le16(
    uint8_t *buf,
    uint16_t value
)
{
    buf[0] =
        (uint8_t)(value & 0xFF);

    buf[1] =
        (uint8_t)((value >> 8) & 0xFF);
}


static void write_le64(
    uint8_t *buf,
    uint64_t value
)
{
    for (int i = 0; i < 8; i++) {

        buf[i] =
            (uint8_t)(
                (value >> (8 * i)) &
                0xFF
            );
    }
}


static void write_be16(
    uint8_t *buf,
    uint16_t value
)
{
    buf[0] =
        (uint8_t)((value >> 8) & 0xFF);

    buf[1] =
        (uint8_t)(value & 0xFF);
}


static void write_be32(
    uint8_t *buf,
    uint32_t value
)
{
    buf[0] =
        (uint8_t)((value >> 24) & 0xFF);

    buf[1] =
        (uint8_t)((value >> 16) & 0xFF);

    buf[2] =
        (uint8_t)((value >> 8) & 0xFF);

    buf[3] =
        (uint8_t)(value & 0xFF);
}


/*
 * ============================================================
 * CRC-16 / CCITT-FALSE
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
                        (crc << 1) ^
                        0x1021
                    );

            } else {

                crc =
                    (uint16_t)(
                        crc << 1
                    );
            }
        }
    }


    return crc;
}


/*
 * ============================================================
 * Sleep millisecond
 * ============================================================
 */

static void sleep_ms(
    int milliseconds
)
{
    if (milliseconds <= 0) {

        return;
    }


    struct timespec req;


    req.tv_sec =
        milliseconds / 1000;

    req.tv_nsec =
        (long)(
            milliseconds % 1000
        ) * 1000000L;


    while (
        nanosleep(
            &req,
            &req
        ) == -1 &&
        errno == EINTR
    ) {
    }
}


/*
 * Motion / Door:
 *
 * period_ms là khoảng thời gian trung bình,
 * nên tạo độ trễ ngẫu nhiên khoảng 50% -> 150%.
 */
static void sleep_sensor_period(
    sensor_type_t type,
    int period_ms
)
{
    if (
        type == SENSOR_MOTION ||
        type == SENSOR_DOOR
    ) {

        int percent =
            50 + rand() % 101;


        int delay =
            period_ms *
            percent /
            100;


        sleep_ms(delay);

        return;
    }


    sleep_ms(period_ms);
}


/*
 * ============================================================
 * Chuỗi tên sensor
 * ============================================================
 */

static const char *sensor_type_name(
    sensor_type_t type
)
{
    switch (type) {

        case SENSOR_TEMPERATURE:
            return "temperature";

        case SENSOR_HUMIDITY:
            return "humidity";

        case SENSOR_MOTION:
            return "motion";

        case SENSOR_DOOR:
            return "door";

        case SENSOR_SMOKE_GAS:
            return "smoke_gas";

        default:
            return "unknown";
    }
}


/*
 * Xác định:
 *
 * sensor type
 * port offset
 */
static int parse_sensor_type(
    const char *name,
    sensor_type_t *type,
    uint16_t *port_offset
)
{
    if (
        strcmp(
            name,
            "temperature"
        ) == 0
    ) {

        *type =
            SENSOR_TEMPERATURE;

        *port_offset = 0;

        return 0;
    }


    if (
        strcmp(
            name,
            "humidity"
        ) == 0
    ) {

        *type =
            SENSOR_HUMIDITY;

        *port_offset = 1;

        return 0;
    }


    if (
        strcmp(
            name,
            "motion"
        ) == 0
    ) {

        *type =
            SENSOR_MOTION;

        *port_offset = 2;

        return 0;
    }


    if (
        strcmp(
            name,
            "door"
        ) == 0
    ) {

        *type =
            SENSOR_DOOR;

        *port_offset = 2;

        return 0;
    }


    if (
        strcmp(
            name,
            "smoke_gas"
        ) == 0
    ) {

        *type =
            SENSOR_SMOKE_GAS;

        *port_offset = 3;

        return 0;
    }


    return -1;
}


/*
 * ============================================================
 * Random double
 * ============================================================
 */

static double random_double(
    double min,
    double max
)
{
    double ratio =
        (double)rand() /
        (double)RAND_MAX;


    return
        min +
        ratio *
        (max - min);
}


/*
 * ============================================================
 * TBP
 *
 * Temperature
 * ============================================================
 */

static size_t build_tbp(
    sensor_id_t id,
    int64_t timestamp,
    bool corrupt,
    uint8_t *frame,
    double *value_out
)
{
    double temperature =
        random_double(
            18.0,
            30.0
        );


    /*
     * Khi --corrupt:
     *
     * tạo temperature ngoài dải hợp lệ.
     */
    if (corrupt) {

        temperature = 200.0;
    }


    write_le16(
        frame,
        id
    );


    uint64_t value_bits;


    memcpy(
        &value_bits,
        &temperature,
        sizeof(value_bits)
    );


    write_le64(
        frame + 2,
        value_bits
    );


    write_le64(
        frame + 10,
        (uint64_t)timestamp
    );


    *value_out =
        temperature;


    return TBP_FRAME_SIZE;
}


/*
 * ============================================================
 * HLP
 *
 * HUM <id> <value> <ts>\n
 * ============================================================
 */

static size_t build_hlp(
    sensor_id_t id,
    int64_t timestamp,
    bool corrupt,
    uint8_t *frame,
    double *value_out
)
{
    double humidity =
        random_double(
            20.0,
            85.0
        );


    int length;


    if (corrupt) {

        /*
         * Prefix sai.
         */
        length =
            snprintf(
                (char *)frame,
                HLP_MAX_FRAME,
                "BAD %u %.1f %lld\n",
                id,
                humidity,
                (long long)timestamp
            );

    } else {

        length =
            snprintf(
                (char *)frame,
                HLP_MAX_FRAME,
                "HUM %u %.1f %lld\n",
                id,
                humidity,
                (long long)timestamp
            );
    }


    if (
        length < 0 ||
        length >= HLP_MAX_FRAME
    ) {

        return 0;
    }


    *value_out =
        humidity;


    return (size_t)length;
}


/*
 * ============================================================
 * EVP Motion
 *
 * type   = 0x01
 * length = 6
 *
 * payload:
 * id uint16
 * ts uint32
 * ============================================================
 */

static size_t build_motion_evp(
    sensor_id_t id,
    uint32_t timestamp,
    bool corrupt,
    uint8_t *frame,
    double *value_out
)
{
    frame[0] = 0x01;


    /*
     * Length đúng là 6.
     */
    frame[1] =
        corrupt
        ? 5
        : 6;


    write_be16(
        frame + 2,
        id
    );


    write_be32(
        frame + 4,
        timestamp
    );


    *value_out = 1.0;


    return 8;
}


/*
 * ============================================================
 * EVP Door
 *
 * type   = 0x02
 * length = 7
 *
 * payload:
 * id
 * state
 * ts
 * ============================================================
 */

static size_t build_door_evp(
    sensor_id_t id,
    uint32_t timestamp,
    uint8_t state,
    bool corrupt,
    uint8_t *frame,
    double *value_out
)
{
    frame[0] = 0x02;


    frame[1] =
        corrupt
        ? 6
        : 7;


    write_be16(
        frame + 2,
        id
    );


    frame[4] =
        state;


    write_be32(
        frame + 5,
        timestamp
    );


    *value_out =
        (double)state;


    return 9;
}


/*
 * ============================================================
 * SGP
 *
 * magic | id | ppm | ts | CRC
 * ============================================================
 */

static size_t build_sgp(
    sensor_id_t id,
    uint32_t timestamp,
    bool corrupt,
    uint8_t *frame,
    double *value_out
)
{
    uint16_t ppm =
        (uint16_t)(
            50 +
            rand() % 451
        );


    frame[0] =
        SGP_MAGIC;


    write_be16(
        frame + 1,
        id
    );


    write_be16(
        frame + 3,
        ppm
    );


    write_be32(
        frame + 5,
        timestamp
    );


    /*
     * CRC tính offset 1 -> 8.
     */
    uint16_t crc =
        crc16_ccitt_false(
            frame + 1,
            8
        );


    /*
     * --corrupt:
     *
     * cố tình phá CRC.
     *
     * Như vậy gateway có thể NAK
     * đúng một frame hoàn chỉnh.
     */
    if (corrupt) {

        crc ^= 0x0001;
    }


    write_be16(
        frame + 9,
        crc
    );


    *value_out =
        (double)ppm;


    return SGP_FRAME_SIZE;
}


/*
 * ============================================================
 * Build frame theo sensor type
 * ============================================================
 */

static size_t build_frame(
    sensor_type_t type,
    sensor_id_t id,
    int64_t timestamp,
    uint8_t *door_state,
    bool corrupt,
    uint8_t *frame,
    double *value_out
)
{
    switch (type) {

        case SENSOR_TEMPERATURE:

            return build_tbp(
                id,
                timestamp,
                corrupt,
                frame,
                value_out
            );


        case SENSOR_HUMIDITY:

            return build_hlp(
                id,
                timestamp,
                corrupt,
                frame,
                value_out
            );


        case SENSOR_MOTION:

            return build_motion_evp(
                id,
                (uint32_t)timestamp,
                corrupt,
                frame,
                value_out
            );


        case SENSOR_DOOR:

            *door_state =
                (uint8_t)(
                    !(*door_state)
                );


            return build_door_evp(
                id,
                (uint32_t)timestamp,
                *door_state,
                corrupt,
                frame,
                value_out
            );


        case SENSOR_SMOKE_GAS:

            return build_sgp(
                id,
                (uint32_t)timestamp,
                corrupt,
                frame,
                value_out
            );


        default:

            return 0;
    }
}


/*
 * ============================================================
 * send() toàn bộ buffer
 * ============================================================
 */

static int send_all(
    int fd,
    const uint8_t *buffer,
    size_t length
)
{
    size_t sent_total = 0;


    while (
        sent_total <
        length
    ) {

        ssize_t sent =
            send(
                fd,
                buffer + sent_total,
                length - sent_total,
                MSG_NOSIGNAL
            );


        if (sent < 0) {

            if (errno == EINTR) {

                continue;
            }


            return -1;
        }


        if (sent == 0) {

            return -1;
        }


        sent_total +=
            (size_t)sent;
    }


    return 0;
}


/*
 * ============================================================
 * --fragment
 *
 * Chia một frame thành 3 lần send().
 * ============================================================
 */

static int send_fragmented(
    int fd,
    const uint8_t *frame,
    size_t length
)
{
    if (length < 3) {

        return send_all(
            fd,
            frame,
            length
        );
    }


    size_t first =
        length / 3;

    size_t second =
        length / 3;

    size_t third =
        length -
        first -
        second;


    if (
        send_all(
            fd,
            frame,
            first
        ) != 0
    ) {

        return -1;
    }


    sleep_ms(30);


    if (
        send_all(
            fd,
            frame + first,
            second
        ) != 0
    ) {

        return -1;
    }


    sleep_ms(30);


    if (
        send_all(
            fd,
            frame +
            first +
            second,
            third
        ) != 0
    ) {

        return -1;
    }


    return 0;
}


/*
 * ============================================================
 * Gửi một frame
 * ============================================================
 */

static int send_frame(
    int fd,
    const uint8_t *frame,
    size_t length,
    bool fragment
)
{
    if (fragment) {

        return send_fragmented(
            fd,
            frame,
            length
        );
    }


    return send_all(
        fd,
        frame,
        length
    );
}


/*
 * ============================================================
 * --glue
 *
 * Gộp hai frame liên tiếp thành một buffer
 * rồi gửi.
 * ============================================================
 */

static int send_glued(
    int fd,
    const uint8_t *frame1,
    size_t len1,
    const uint8_t *frame2,
    size_t len2
)
{
    uint8_t buffer[
        MAX_FRAME_SIZE * 2
    ];


    if (
        len1 + len2 >
        sizeof(buffer)
    ) {

        return -1;
    }


    memcpy(
        buffer,
        frame1,
        len1
    );


    memcpy(
        buffer + len1,
        frame2,
        len2
    );


    return send_all(
        fd,
        buffer,
        len1 + len2
    );
}


/*
 * ============================================================
 * Chờ ACK / NAK SGP
 *
 * timeout = 1 giây
 * ============================================================
 */

static int wait_sgp_reply(
    int fd
)
{
    struct pollfd pfd;


    memset(
        &pfd,
        0,
        sizeof(pfd)
    );


    pfd.fd =
        fd;

    pfd.events =
        POLLIN;


    int result;


    do {

        result =
            poll(
                &pfd,
                1,
                1000
            );

    } while (
        result < 0 &&
        errno == EINTR
    );


    if (result <= 0) {

        return 0;
    }


    uint8_t reply;


    ssize_t received;


    do {

        received =
            recv(
                fd,
                &reply,
                1,
                0
            );

    } while (
        received < 0 &&
        errno == EINTR
    );


    if (received != 1) {

        return -1;
    }


    if (reply == SGP_ACK) {

        return 1;
    }


    if (reply == SGP_NAK) {

        return -1;
    }


    return -1;
}


/*
 * ============================================================
 * Gửi SGP + retry ACK/NAK
 *
 * Initial send + tối đa 3 lần retry.
 * ============================================================
 */

static int send_sgp_with_retry(
    int fd,
    const uint8_t *frame,
    size_t length,
    bool fragment
)
{
    for (
        int attempt = 0;
        attempt <= SGP_MAX_RETRIES;
        attempt++
    ) {

        if (
            send_frame(
                fd,
                frame,
                length,
                fragment
            ) != 0
        ) {

            return -1;
        }


        int reply =
            wait_sgp_reply(fd);


        if (reply == 1) {

            return 0;
        }


        if (
            attempt <
            SGP_MAX_RETRIES
        ) {

            printf(
                "SGP NAK/timeout -> retry %d\n",
                attempt + 1
            );
        }
    }


    printf(
        "SGP frame failed after retries\n"
    );


    /*
     * Không đóng toàn connection.
     * Tiếp tục gửi measurement sau.
     */
    return 0;
}


/*
 * ============================================================
 * In measurement ra stdout
 * ============================================================
 */

static void print_measurement(
    sensor_id_t id,
    sensor_type_t type,
    double value,
    bool corrupted
)
{
    printf(
        "sensor %u %-11s value=%7.2f%s\n",
        id,
        sensor_type_name(type),
        value,
        corrupted
            ? " [CORRUPT]"
            : ""
    );
}


/*
 * ============================================================
 * Timestamp để in trạng thái kết nối
 * ============================================================
 */

static void print_connection_message(
    sensor_id_t id,
    sensor_type_t type,
    const char *host,
    uint16_t port
)
{
    time_t now =
        time(NULL);


    struct tm tm_now;


    localtime_r(
        &now,
        &tm_now
    );


    char timestamp[32];


    strftime(
        timestamp,
        sizeof(timestamp),
        "%Y-%m-%d %H:%M:%S",
        &tm_now
    );


    printf(
        "%s sensor %u %s connected to %s:%u\n",
        timestamp,
        id,
        sensor_type_name(type),
        host,
        port
    );
}


/*
 * ============================================================
 * Usage
 * ============================================================
 */

static void print_usage(
    const char *program
)
{
    fprintf(
        stderr,
        "Usage:\n"
        "%s <type> <id> <host> <base_port> <period_ms> [options]\n"
        "\n"
        "type:\n"
        "  temperature\n"
        "  humidity\n"
        "  motion\n"
        "  door\n"
        "  smoke_gas\n"
        "\n"
        "options:\n"
        "  --fragment\n"
        "  --glue\n"
        "  --corrupt\n",
        program
    );
}


/*
 * ============================================================
 * MAIN
 * ============================================================
 */

int main(
    int argc,
    char *argv[]
)
{
    if (
        argc < 6 ||
        argc > 9
    ) {

        print_usage(
            argv[0]
        );

        return 1;
    }


    sensor_type_t type;

    uint16_t port_offset;


    if (
        parse_sensor_type(
            argv[1],
            &type,
            &port_offset
        ) != 0
    ) {

        fprintf(
            stderr,
            "Unknown sensor type: %s\n",
            argv[1]
        );

        return 1;
    }


    unsigned long id_value =
        strtoul(
            argv[2],
            NULL,
            10
        );


    if (id_value > UINT16_MAX) {

        fprintf(
            stderr,
            "Invalid sensor ID\n"
        );

        return 1;
    }


    sensor_id_t sensor_id =
        (sensor_id_t)id_value;


    const char *host =
        argv[3];


    unsigned long base_port_value =
        strtoul(
            argv[4],
            NULL,
            10
        );


    if (
        base_port_value == 0 ||
        base_port_value +
        port_offset >
        UINT16_MAX
    ) {

        fprintf(
            stderr,
            "Invalid base port\n"
        );

        return 1;
    }


    uint16_t port =
        (uint16_t)(
            base_port_value +
            port_offset
        );


    int period_ms =
        atoi(
            argv[5]
        );


    if (period_ms <= 0) {

        fprintf(
            stderr,
            "Invalid period\n"
        );

        return 1;
    }


    /*
     * ========================================================
     * Options
     * ========================================================
     */

    bool option_fragment =
        false;

    bool option_glue =
        false;

    bool option_corrupt =
        false;


    for (
        int i = 6;
        i < argc;
        i++
    ) {

        if (
            strcmp(
                argv[i],
                "--fragment"
            ) == 0
        ) {

            option_fragment =
                true;

        } else if (
            strcmp(
                argv[i],
                "--glue"
            ) == 0
        ) {

            option_glue =
                true;

        } else if (
            strcmp(
                argv[i],
                "--corrupt"
            ) == 0
        ) {

            option_corrupt =
                true;

        } else {

            fprintf(
                stderr,
                "Unknown option: %s\n",
                argv[i]
            );

            return 1;
        }
    }


    /*
     * Fragment và glue là hai cách test
     * ngược nhau, không dùng đồng thời.
     */
    if (
        option_fragment &&
        option_glue
    ) {

        fprintf(
            stderr,
            "--fragment and --glue cannot be used together\n"
        );

        return 1;
    }


    /*
     * ========================================================
     * TCP connection
     * ========================================================
     */

    int sockfd =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );


    if (sockfd == -1) {

        perror("socket");

        return 1;
    }


    struct sockaddr_in server_addr;


    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );


    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(port);


    if (
        inet_pton(
            AF_INET,
            host,
            &server_addr.sin_addr
        ) != 1
    ) {

        fprintf(
            stderr,
            "Invalid IP address\n"
        );


        close(sockfd);

        return 1;
    }


    if (
        connect(
            sockfd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) == -1
    ) {

        perror("connect");

        close(sockfd);

        return 1;
    }


    print_connection_message(
        sensor_id,
        type,
        host,
        port
    );


    srand(
        (unsigned int)(
            time(NULL) ^
            sensor_id ^
            getpid()
        )
    );


    uint8_t door_state = 0;

    unsigned long frame_counter = 0;


    /*
     * ========================================================
     * Measurement loop
     * ========================================================
     */

    while (1) {

        /*
         * ----------------------------------------------------
         * --glue
         *
         * Tạo hai frame rồi gửi cùng nhau.
         * ----------------------------------------------------
         */

        if (option_glue) {

            uint8_t frame1[
                MAX_FRAME_SIZE
            ];

            uint8_t frame2[
                MAX_FRAME_SIZE
            ];


            int64_t ts1 =
                (int64_t)time(NULL);

            int64_t ts2 =
                ts1 + 1;


            frame_counter++;


            bool corrupt1 =
                option_corrupt &&
                (
                    frame_counter %
                    5 == 0
                );


            double value1;


            size_t len1 =
                build_frame(
                    type,
                    sensor_id,
                    ts1,
                    &door_state,
                    corrupt1,
                    frame1,
                    &value1
                );


            frame_counter++;


            bool corrupt2 =
                option_corrupt &&
                (
                    frame_counter %
                    5 == 0
                );


            double value2;


            size_t len2 =
                build_frame(
                    type,
                    sensor_id,
                    ts2,
                    &door_state,
                    corrupt2,
                    frame2,
                    &value2
                );


            if (
                len1 == 0 ||
                len2 == 0
            ) {

                fprintf(
                    stderr,
                    "Failed to build frame\n"
                );

                break;
            }


            /*
             * SGP cần ACK cho từng frame.
             */
            if (
                type ==
                SENSOR_SMOKE_GAS
            ) {

                if (
                    send_glued(
                        sockfd,
                        frame1,
                        len1,
                        frame2,
                        len2
                    ) != 0
                ) {

                    perror("send");

                    break;
                }


                int reply1 =
                    wait_sgp_reply(
                        sockfd
                    );


                int reply2 =
                    wait_sgp_reply(
                        sockfd
                    );


                /*
                 * Frame nào fail thì gửi lại
                 * riêng frame đó.
                 */
                if (reply1 != 1) {

                    if (
                        send_sgp_with_retry(
                            sockfd,
                            frame1,
                            len1,
                            false
                        ) != 0
                    ) {

                        break;
                    }
                }


                if (reply2 != 1) {

                    if (
                        send_sgp_with_retry(
                            sockfd,
                            frame2,
                            len2,
                            false
                        ) != 0
                    ) {

                        break;
                    }
                }

            } else {

                if (
                    send_glued(
                        sockfd,
                        frame1,
                        len1,
                        frame2,
                        len2
                    ) != 0
                ) {

                    perror("send");

                    break;
                }
            }


            print_measurement(
                sensor_id,
                type,
                value1,
                corrupt1
            );


            print_measurement(
                sensor_id,
                type,
                value2,
                corrupt2
            );
        }


        /*
         * ----------------------------------------------------
         * Normal / --fragment
         * ----------------------------------------------------
         */

        else {

            uint8_t frame[
                MAX_FRAME_SIZE
            ];


            int64_t timestamp =
                (int64_t)time(NULL);


            frame_counter++;


            /*
             * Mỗi frame thứ 5 sẽ bị corrupt.
             */
            bool corrupted =
                option_corrupt &&
                (
                    frame_counter %
                    5 == 0
                );


            double value;


            size_t frame_len =
                build_frame(
                    type,
                    sensor_id,
                    timestamp,
                    &door_state,
                    corrupted,
                    frame,
                    &value
                );


            if (frame_len == 0) {

                fprintf(
                    stderr,
                    "Failed to build frame\n"
                );

                break;
            }


            /*
             * SGP có ACK/NAK.
             */
            if (
                type ==
                SENSOR_SMOKE_GAS
            ) {

                if (
                    send_sgp_with_retry(
                        sockfd,
                        frame,
                        frame_len,
                        option_fragment
                    ) != 0
                ) {

                    perror("send");

                    break;
                }

            } else {

                if (
                    send_frame(
                        sockfd,
                        frame,
                        frame_len,
                        option_fragment
                    ) != 0
                ) {

                    perror("send");

                    break;
                }
            }


            print_measurement(
                sensor_id,
                type,
                value,
                corrupted
            );
        }


        sleep_sensor_period(
            type,
            period_ms
        );
    }


    close(
        sockfd
    );


    return 0;
}