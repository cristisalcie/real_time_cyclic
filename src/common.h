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

#define STRING_SIZE 1024
#define SHARED_MEMORY_KEY 0x1234

#define STRING_PARAMETER_BIT 1 << 0 // 0001
#define INT_PARAMETER_BIT 1 << 1    // 0010
#define BOOL_PARAMETER_BIT 1 << 2   // 0100

#define STRING_VALUE_UNDEFINED 0
#define INT_VALUE_UNDEFINED -1
#define BOOL_VALUE_UNDEFINED -1

#define WAIT_TIMEOUT_SECONDS 3
#define ERROR_NOT_SET_COMMUNICATION_CYCLE_MS -1

#define SHMSEG_ERROR_SIZE 20

typedef enum {
    START_MASTER,
    STOP_MASTER,        // TODO
    CREATE_SLAVE,
    DELETE_SLAVE,       // TODO
    CONNECT_SLAVE,
    DISCONNECT_SLAVE,
    START_SLAVE_CYCLE,
    STOP_SLAVE_CYCLE,
    PRINT_SLAVE_DATA,
    AUTOMATIC_TEST,
    CHANGE_SLAVE_NAME,
    SIGNAL_MASTER_PARAMETER,  // Used by slave in request
    NO_REQUEST,
    REQUEST_SIZE
} request_t;

typedef enum {
    NACK,
    ACK,
    NO_RESPONSE,
    RESPONSE_SIZE
} response_t;

typedef struct shmseg_error_s {
    char error_string[STRING_SIZE];
} shmseg_error_t;

typedef struct shmseg_s {
    // Fields modifiable only by slaves
    char name[SLAVE_NAME_SIZE];  // Name string can't contain spaces
    request_t req_s_to_m;
    response_t res_s_to_m;
    int available_parameters;

    // Fields modifiable only by master
    pid_t pid;  // Master assigns shared memory index by setting pid of shared memory segment owner
    request_t req_m_to_s;
    response_t res_m_to_s;
    bool is_connected;

    // Modifiable only by configurator
    int requested_parameters;
    long int communication_cycle_us;

    // Fields modifiable by master and slave
    // Slave sets them, master reads and resets them to a DEFINE value in order for master to know
    // whether requested parameters got sent in next the cycle.
    bool cycle_started;
    shmseg_error_t shmseg_error[SHMSEG_ERROR_SIZE];  // Slave sets, Master resets when consumed
    u_int16_t shmseg_error_current_size;
    char string_value[STRING_SIZE];
    int int_value;
    bool bool_value;
} shmseg_t;

typedef struct shm_s {
    // Using hashmap logic on slave_shmseg vector by doing slave_pid % MAX_SLAVES
    // while slave_shmseg[slave_pid % MAX_SLAVES].pid != slave_pid
    // index++;
    // index % MAX_SLAVES
    shmseg_t slave_shmseg[MAX_SLAVES];
    pid_t master_pid;
} shm_t;

#endif /* COMMON_H */
