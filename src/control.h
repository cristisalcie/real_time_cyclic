#ifndef CONTROL_H
#define CONTROL_H

#define CONTROL_SHARED_MEMORY_KEY 0x4321

typedef struct control_shm_s {
    // Current convention: configurator sends request and master sends response
    request_t request;
    response_t response;
    pid_t configurator_pid;
    pid_t connect_disconnect_slave_pid;
} control_shm_t;

#endif /* CONTROL_H */
