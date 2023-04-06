#ifndef CONFIGURATOR_H
#define CONFIGURATOR_H

#include "common.h"
#include "control.h"

#define MASTER_BIN_PATH "/home/ubuntu/Projects/real_time_cyclic/out/master"
#define SLAVE_BIN_PATH "/home/ubuntu/Projects/real_time_cyclic/out/slave"
#define MAX_LINE_LENGTH 16

typedef struct created_slave_s {
    pid_t pid;
    bool connected;
} created_slave_t;

typedef struct configurator_context_s {
    shm_t *shmp;  // Configurator can modify requested_parameters and read everything else
    control_shm_t *control_shmp;
    pid_t master_pid;
    int created_slaves_number;
    created_slave_t created_slave[MAX_SLAVES];
} configurator_context_t;

int send_start_cycle_slave_request(pid_t slave_pid);
int send_stop_cycle_slave_request(pid_t slave_pid);
int send_connect_slave_request(pid_t slave_pid);
int send_disconnect_slave_request(pid_t slave_pid);

void wait_start_slave_cycle_response();
void wait_stop_slave_cycle_response();
void wait_connect_slave_response(pid_t slave_pid);
void wait_disconnect_slave_response(pid_t slave_pid);
int wait_master_started_signal();

#endif /* CONFIGURATOR_H */
