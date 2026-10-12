#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#include "connection_manager.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"
#include "sensor_types.h"
#include "protocol_adapter.h"


#define LISTENER_COUNT 4
#define RX_BUFFER_SIZE 256


/*
 * ============================================================
 * Bảng protocol adapter
 *
 * index 0 -> P     -> TBP
 * index 1 -> P + 1 -> HLP
 * index 2 -> P + 2 -> EVP
 * index 3 -> P + 3 -> SGP
 * ============================================================
 */

static const protocol_adapter_t *const ADAPTERS[
    LISTENER_COUNT
] = {
    &TBP_ADAPTER,
    &HLP_ADAPTER,
    &EVP_ADAPTER,
    &SGP_ADAPTER
};


/*
 * Mỗi TCP connection có:
 *
 * - socket riêng
 * - adapter riêng
 * - receive buffer riêng
 */
typedef struct {

    int fd;

    const protocol_adapter_t *adapter;

    uint8_t rx[RX_BUFFER_SIZE];

    size_t rx_len;

    bool identified;

    sensor_id_t sensor_id;

} client_t;


/*
 * ============================================================
 * Kiểm tra gateway stop
 * ============================================================
 */

static bool gateway_should_stop(
    gateway_t *gateway
)
{
    bool stop;


    pthread_mutex_lock(
        &gateway->stop_mutex
    );


    stop =
        gateway->stop;


    pthread_mutex_unlock(
        &gateway->stop_mutex
    );


    return stop;
}


/*
 * ============================================================
 * Tạo một listening socket
 * ============================================================
 */

static int create_listener(
    uint16_t port
)
{
    int fd =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );


    if (fd == -1) {

        perror("socket");

        return -1;
    }


    int reuse = 1;


    if (
        setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) == -1
    ) {

        perror("setsockopt");

        close(fd);

        return -1;
    }


    struct sockaddr_in address;


    memset(
        &address,
        0,
        sizeof(address)
    );


    address.sin_family =
        AF_INET;

    address.sin_addr.s_addr =
        htonl(INADDR_ANY);

    address.sin_port =
        htons(port);


    if (
        bind(
            fd,
            (struct sockaddr *)&address,
            sizeof(address)
        ) == -1
    ) {

        perror("bind");

        close(fd);

        return -1;
    }


    if (
        listen(
            fd,
            SOMAXCONN
        ) == -1
    ) {

        perror("listen");

        close(fd);

        return -1;
    }


    return fd;
}


/*
 * ============================================================
 * Log lỗi accept
 * ============================================================
 */

static void log_accept_error(
    const protocol_adapter_t *adapter
)
{
    char message[200];


    snprintf(
        message,
        sizeof(message),
        "Connection attempt failed on %s port (accept error)",
        adapter->name
    );


    logger_send(
        "CONN",
        message
    );
}


/*
 * ============================================================
 * Log malformed frame
 * ============================================================
 */

static void log_malformed_frame(
    const protocol_adapter_t *adapter,
    const char *reason
)
{
    char message[200];


    snprintf(
        message,
        sizeof(message),
        "Received malformed frame on %s port (%s)",
        adapter->name,
        reason
    );


    logger_send(
        "CONN",
        message
    );
}


/*
 * ============================================================
 * Xác định reason cơ bản khi parse lỗi
 * ============================================================
 */

static const char *parse_error_reason(
    const client_t *client
)
{
    /*
     * Frame hợp lệ đầu tiên còn chưa nhận được.
     */
    if (!client->identified) {

        return "malformed first frame";
    }


    /*
     * Với SGP lỗi thường liên quan magic / CRC.
     */
    if (
        strcmp(
            client->adapter->name,
            "SGP"
        ) == 0
    ) {

        return "bad CRC";
    }


    return "bad length";
}


/*
 * ============================================================
 * Gửi reply
 *
 * Chủ yếu được dùng bởi SGP.
 * ============================================================
 */

static int send_reply(
    int fd,
    const uint8_t *reply,
    size_t len
)
{
    size_t sent_total = 0;


    while (sent_total < len) {

        ssize_t sent =
            send(
                fd,
                reply + sent_total,
                len - sent_total,
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
 * Xóa các byte đã parse khỏi RX buffer
 * ============================================================
 */

static void consume_rx(
    client_t *client,
    size_t consumed
)
{
    if (
        consumed == 0 ||
        consumed > client->rx_len
    ) {

        return;
    }


    client->rx_len -=
        consumed;


    if (client->rx_len > 0) {

        memmove(
            client->rx,
            client->rx + consumed,
            client->rx_len
        );
    }
}


/*
 * ============================================================
 * Đóng connection sensor
 * ============================================================
 */

static void remove_client(
    client_t *clients,
    size_t *count,
    size_t index
)
{
    client_t *client =
        &clients[index];


    if (client->identified) {

        char message[200];


        snprintf(
            message,
            sizeof(message),
            "The sensor node with %u has closed the connection",
            client->sensor_id
        );


        logger_send(
            "CONN",
            message
        );
    }


    close(
        client->fd
    );


    /*
     * Dồn mảng client lại.
     */
    if (
        index <
        *count - 1
    ) {

        memmove(
            &clients[index],
            &clients[index + 1],
            (*count - index - 1) *
            sizeof(client_t)
        );
    }


    (*count)--;
}


/*
 * ============================================================
 * CONNECTION MANAGER THREAD
 * ============================================================
 */

void *connection_manager_thread(
    void *arg
)
{
    gateway_t *gateway =
        (gateway_t *)arg;


#ifdef DEBUG

    printf(
        "[CONN] Connection manager thread started\n"
    );

#endif


    logger_send(
        "CONN",
        "Connection manager thread started"
    );


    /*
     * ========================================================
     * Tạo 4 listening socket
     * ========================================================
     */

    int listen_fd[
        LISTENER_COUNT
    ];


    for (
        size_t i = 0;
        i < LISTENER_COUNT;
        i++
    ) {

        listen_fd[i] = -1;
    }


    for (
        size_t i = 0;
        i < LISTENER_COUNT;
        i++
    ) {

        unsigned int port_value =
            (unsigned int)gateway->base_port +
            ADAPTERS[i]->port_offset;


        if (port_value > UINT16_MAX) {

            fprintf(
                stderr,
                "Invalid sensor port\n"
            );


            goto shutdown_listeners;
        }


        uint16_t port =
            (uint16_t)port_value;


        listen_fd[i] =
            create_listener(port);


        if (listen_fd[i] == -1) {

            goto shutdown_listeners;
        }


#ifdef DEBUG

        printf(
            "[CONN] Listening %s on port %u\n",
            ADAPTERS[i]->name,
            port
        );

#endif
    }


    /*
     * ========================================================
     * Danh sách sensor connection động
     * ========================================================
     */

    client_t *clients =
        NULL;

    size_t client_count =
        0;


    /*
     * ========================================================
     * Event loop
     * ========================================================
     */

    while (
        !gateway_should_stop(
            gateway
        )
    ) {

        /*
         * Quan trọng:
         *
         * ghi lại số client có tại thời điểm
         * tạo mảng poll.
         *
         * Sau poll có thể accept thêm client mới,
         * nhưng client mới không có entry trong
         * fds hiện tại.
         */
        size_t polled_client_count =
            client_count;


        size_t poll_count =
            LISTENER_COUNT +
            polled_client_count;


        struct pollfd *fds =
            calloc(
                poll_count,
                sizeof(struct pollfd)
            );


        if (fds == NULL) {

            break;
        }


        /*
         * ----------------------------------------------------
         * 4 listening sockets
         * ----------------------------------------------------
         */

        for (
            size_t i = 0;
            i < LISTENER_COUNT;
            i++
        ) {

            fds[i].fd =
                listen_fd[i];

            fds[i].events =
                POLLIN;
        }


        /*
         * ----------------------------------------------------
         * Các sensor connection
         * ----------------------------------------------------
         */

        for (
            size_t i = 0;
            i < polled_client_count;
            i++
        ) {

            fds[
                LISTENER_COUNT + i
            ].fd =
                clients[i].fd;


            fds[
                LISTENER_COUNT + i
            ].events =
                POLLIN;
        }


        /*
         * Timeout 500 ms để kiểm tra stop.
         */
        int result =
            poll(
                fds,
                poll_count,
                500
            );


        if (result < 0) {

            free(fds);


            if (errno == EINTR) {

                continue;
            }


            perror("poll");

            break;
        }


        if (result == 0) {

            free(fds);

            continue;
        }


        /*
         * ====================================================
         * Xử lý sensor mới trên 4 cổng
         * ====================================================
         */

        for (
            size_t listener_index = 0;
            listener_index < LISTENER_COUNT;
            listener_index++
        ) {

            if (
                !(
                    fds[
                        listener_index
                    ].revents &
                    POLLIN
                )
            ) {

                continue;
            }


            int client_fd =
                accept(
                    listen_fd[
                        listener_index
                    ],
                    NULL,
                    NULL
                );


            if (client_fd == -1) {

                log_accept_error(
                    ADAPTERS[
                        listener_index
                    ]
                );


                continue;
            }


            client_t *new_clients =
                realloc(
                    clients,
                    (
                        client_count + 1
                    ) *
                    sizeof(client_t)
                );


            if (new_clients == NULL) {

                close(
                    client_fd
                );


                continue;
            }


            clients =
                new_clients;


            client_t *client =
                &clients[
                    client_count
                ];


            memset(
                client,
                0,
                sizeof(*client)
            );


            client->fd =
                client_fd;


            /*
             * Quan trọng:
             *
             * Sensor kết nối vào port nào
             * thì connection đó dùng adapter
             * của port đó.
             */
            client->adapter =
                ADAPTERS[
                    listener_index
                ];


            client_count++;


#ifdef DEBUG

            printf(
                "[CONN] New connection on %s port\n",
                client->adapter->name
            );

#endif
        }


        /*
         * ====================================================
         * Xử lý dữ liệu client cũ
         *
         * Đi từ cuối về đầu vì remove_client()
         * có memmove.
         * ====================================================
         */

        for (
            size_t pos =
                polled_client_count;

            pos > 0;

            pos--
        ) {

            size_t i =
                pos - 1;


            short events =
                fds[
                    LISTENER_COUNT + i
                ].revents;


            if (events == 0) {

                continue;
            }


            bool close_connection =
                false;


            /*
             * ------------------------------------------------
             * Có dữ liệu
             * ------------------------------------------------
             */

            if (events & POLLIN) {

                client_t *client =
                    &clients[i];


                /*
                 * Buffer đầy mà parser vẫn chưa
                 * lấy được frame -> malformed.
                 */
                if (
                    client->rx_len >=
                    sizeof(client->rx)
                ) {

                    log_malformed_frame(
                        client->adapter,
                        "bad length"
                    );


                    close_connection =
                        true;

                } else {

                    ssize_t received =
                        recv(
                            client->fd,

                            client->rx +
                            client->rx_len,

                            sizeof(client->rx) -
                            client->rx_len,

                            0
                        );


                    if (received == 0) {

                        close_connection =
                            true;

                    } else if (
                        received < 0
                    ) {

                        if (
                            errno != EINTR &&
                            errno != EAGAIN
                        ) {

                            close_connection =
                                true;
                        }

                    } else {

                        client->rx_len +=
                            (size_t)received;


                        /*
                         * ====================================
                         * PARSE LOOP
                         *
                         * Một recv() có thể chứa:
                         *
                         * - nửa frame
                         * - đúng 1 frame
                         * - nhiều frame
                         *
                         * nên parse liên tục.
                         * ====================================
                         */

                        while (
                            client->rx_len > 0
                        ) {

                            sensor_data_t data;

                            size_t consumed = 0;


                            parse_result_t parse_result =
                                client->adapter->parse(
                                    client->rx,
                                    client->rx_len,
                                    &data,
                                    &consumed
                                );


                            /*
                             * Chưa đủ frame.
                             *
                             * Giữ nguyên buffer,
                             * chờ recv() tiếp.
                             */
                            if (
                                parse_result ==
                                PARSE_NEED_MORE
                            ) {

                                break;
                            }


                            /*
                             * Adapter SGP có thể tạo
                             * ACK hoặc NAK.
                             */
                            uint8_t reply[16];


                            size_t reply_len =
                                client->
                                adapter->
                                build_reply(
                                    parse_result,
                                    reply,
                                    sizeof(reply)
                                );


                            if (reply_len > 0) {

                                if (
                                    send_reply(
                                        client->fd,
                                        reply,
                                        reply_len
                                    ) != 0
                                ) {

                                    close_connection =
                                        true;

                                    break;
                                }
                            }


                            /*
                             * ----------------------------
                             * FRAME HỢP LỆ
                             * ----------------------------
                             */

                            if (
                                parse_result ==
                                PARSE_OK
                            ) {

                                /*
                                 * Frame hợp lệ đầu tiên
                                 * -> mới biết sensor ID.
                                 */
                                if (
                                    !client->
                                    identified
                                ) {

                                    client->
                                    identified =
                                        true;


                                    client->
                                    sensor_id =
                                        data.id;


                                    char message[
                                        200
                                    ];


                                    snprintf(
                                        message,
                                        sizeof(message),

                                        "A sensor node with %u has opened a new connection",

                                        data.id
                                    );


                                    logger_send(
                                        "CONN",
                                        message
                                    );
                                }


                                /*
                                 * Dữ liệu đã chuẩn hóa:
                                 *
                                 * TBP/HLP/EVP/SGP
                                 *        ↓
                                 * sensor_data_t
                                 *        ↓
                                 * sbuffer
                                 */
                                sbuffer_insert(
                                    &gateway->
                                    sbuffer,

                                    &data
                                );

                            }


                            /*
                             * ----------------------------
                             * FRAME HỎNG
                             * ----------------------------
                             */

                            else {

                                const char *reason =
                                    parse_error_reason(
                                        client
                                    );


                                log_malformed_frame(
                                    client->
                                    adapter,

                                    reason
                                );
                            }


                            /*
                             * Adapter bắt buộc phải
                             * cho biết cần bỏ bao nhiêu
                             * byte.
                             *
                             * Nếu consumed = 0 ở đây
                             * thì parser bị lỗi logic,
                             * nếu tiếp tục sẽ loop vô hạn.
                             */
                            if (
                                consumed == 0 ||
                                consumed >
                                client->rx_len
                            ) {

                                close_connection =
                                    true;

                                break;
                            }


                            consume_rx(
                                client,
                                consumed
                            );
                        }
                    }
                }
            }


            /*
             * Socket error / peer disconnect.
             */
            if (
                events &
                (
                    POLLHUP |
                    POLLERR |
                    POLLNVAL
                )
            ) {

                close_connection =
                    true;
            }


            if (close_connection) {

                remove_client(
                    clients,
                    &client_count,
                    i
                );
            }
        }


        free(fds);
    }


    /*
     * ========================================================
     * Shutdown client
     * ========================================================
     */

    for (
        size_t i = client_count;
        i > 0;
        i--
    ) {

        remove_client(
            clients,
            &client_count,
            i - 1
        );
    }


    free(
        clients
    );


shutdown_listeners:

    /*
     * ========================================================
     * Đóng 4 listening socket
     * ========================================================
     */

    for (
        size_t i = 0;
        i < LISTENER_COUNT;
        i++
    ) {

        if (
            listen_fd[i] != -1
        ) {

            close(
                listen_fd[i]
            );
        }
    }


#ifdef DEBUG

    printf(
        "[CONN] Connection manager thread stopped\n"
    );

#endif


    return NULL;
}