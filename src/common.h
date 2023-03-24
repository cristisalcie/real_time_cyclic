#ifndef COMMON_H
#define COMMON_H

#define SLAVE_NAME_SIZE 128
#define MAX_SLAVES 32

#define RTC_ERROR 1
#define RTC_SUCCESS 0

#define bool __uint8_t
#define false 0
#define true 1
#define NO_PID -1
#define NO_IDX -1

typedef enum {
    START_MASTER,
    STOP_MASTER,
    CREATE_SLAVE,
    DELETE_SLAVE,
    CONNECT_SLAVE,
    DISCONNECT_SLAVE,
    CHANGE_SLAVE_NAME,
    REQUEST_SIZE
} request_t;

typedef enum {
    NACK,
    ACK,
    RESPONSE_SIZE
} response_t;

#endif /* COMMON_H */
