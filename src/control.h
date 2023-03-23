#ifndef CONTROL_H
#define CONTROL_H

#define CONTROL_SHARED_MEMORY_KEY 0x4321

// NOTE: Master never requests to configurator only responds
typedef enum {
    C_RQ_START_MASTER,  // internal request
    C_RQ_STOP_MASTER,
    C_RQ_CREATE_SLAVE,  // internal request
    C_RQ_DELETE_SLAVE,
    C_RQ_CONNECT_SLAVE,
    C_RQ_DISCONNECT_SLAVE,
    C_RQ_CMD_SIZE
} req_control_cmd;

typedef enum {
    C_RS_NACK,
    C_RS_ACK,
    C_RS_CMD_SIZE
} res_control_cmd;

typedef struct control_shm_s {
    req_control_cmd req_c_cmd;
    res_control_cmd res_c_cmd;
    pid_t configurator_pid;
    pid_t connect_disconnect_slave_pid;
} control_shm_t;

#endif /* CONTROL_H */
