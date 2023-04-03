#ifndef SLAVE_H
#define SLAVE_H

#include "common.h"

typedef struct slave_context_s {
    shm_t *shmp;
    char name[SLAVE_NAME_SIZE];
    unsigned int change_name_idx;
    int available_parameters;

    struct timeval last_communication_timeval;
    double excess_communication_cycle_interval_ms;
    // Signal semaphore to allow parameters communication cycle
    sem_t master_processed_communication_cycle;
    sem_t allow_communication_cycle;
    int shmsegIdx;
} slave_context_t;

void handle_connect_slave_request();
int handle_disconnect_slave_request();
void handle_change_name_slave_request();
void handle_start_cycle_slave_request();
void handle_stop_cycle_slave_request();

#endif /* SLAVE_H */
