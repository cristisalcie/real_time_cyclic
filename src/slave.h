#ifndef SLAVE_H
#define SLAVE_H

#include "common.h"

typedef struct slave_context_s {
    shm_t *shmp;
    char name[SLAVE_NAME_SIZE];
    unsigned int change_name_idx;
    int available_parameters;
    bool cycle_started;

    // Signal semaphore to allow parameters communication cycle
    sem_t allow_communication_cycle; // TODO 1
    int shmsegIdx;
} slave_context_t;

#endif /* SLAVE_H */
