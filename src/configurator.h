#ifndef CONFIGURATOR_H
#define CONFIGURATOR_H

#include "common.h"
#include "control.h"

#define MASTER_BIN_PATH "/home/ubuntu/Projects/real_time_cyclic/out/master"
#define SLAVE_BIN_PATH "/home/ubuntu/Projects/real_time_cyclic/out/slave"
#define MAX_LINE_LENGTH 16

typedef struct configurator_context_s {
    shm_t *shmp;  // Configurator can modify requested_parameters and read everything else
    control_shm_t *control_shmp;
    pid_t master_pid;
} configurator_context_t;

#endif /* CONFIGURATOR_H */
