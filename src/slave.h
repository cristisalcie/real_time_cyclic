#ifndef SLAVE_H
#define SLAVE_H

#include "common.h"
#include "m_s_common.h"

typedef struct slave_context_s {
    shm_t *shmp;
    char name[SLAVE_NAME_SIZE];
    unsigned int change_name_idx;

    // Signal semaphore to start timer
    sem_t timer_semaphore;
    int shmsegIdx;
} slave_context_t;

#endif /* SLAVE_H */
