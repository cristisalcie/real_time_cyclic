#ifndef CONTROL_H
#define CONTROL_H

#define CONTROL_SHARED_MEMORY_KEY 0x4321

typedef struct control_shm_s {
    // Current convention: configurator sends request and master sends response

    // Modifiable only by master
    response_t response;

    // Modifiable only by configurator
    request_t request;
    pid_t configurator_pid;
    pid_t affected_slave_pid;
} control_shm_t;

#endif /* CONTROL_H */
