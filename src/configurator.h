#ifndef CONFIGURATOR_H
#define CONFIGURATOR_H

#include "common.h"
#include "control.h"

#define MASTER_BIN_PATH "/home/ubuntu/Projects/real_time_cyclic/out/master"
#define SLAVE_BIN_PATH "/home/ubuntu/Projects/real_time_cyclic/out/slave"
#define MAX_CREATE_SLAVES 64  // TODO

typedef struct slave_data_s {
    char name[SLAVE_NAME_SIZE];
    pid_t pid;
} slave_data_t; // TODO

typedef struct configurator_context_s {
    control_shm_t *shmp;
    pid_t master_pid;

    slave_data_t created_slave_data[MAX_CREATE_SLAVES]; // TODO
    slave_data_t connected_slave_data[MAX_SLAVES];      // TODO
} configurator_context_t;

#endif /* CONFIGURATOR_H */
