#ifndef SLAVE_H
#define SLAVE_H

#include "common.h"

// Bigger value => increased busy waiting
#define WAIT_MASTER_RESPONSE_TIMEOUT_SEGMENTS 20

// Errors do stack!
#define set_error(str, args...)                                                                 \
    do {                                                                                        \
        fprintf(stderr, "[DEBUG %d] " str "\n", getpid(), ##args);                              \
        if (self.shmseg->shmseg_error_current_size < SHMSEG_ERROR_SIZE) {                       \
            snprintf(                                                                           \
                self.shmseg->shmseg_error[self.shmseg->shmseg_error_current_size].error_string, \
                STRING_SIZE,                                                                    \
                str, ##args);                                                                   \
            ++self.shmseg->shmseg_error_current_size;                                           \
        } else {                                                                                \
            fprintf(stderr, "slave[%d] Couldn't set error because error buffer is full!\n",     \
                getpid());                                                                      \
        }                                                                                       \
    } while(0)

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
    shmseg_t *shmseg;
} slave_context_t;

// Returns elapsed time waiting for master response always >= 0
long int wait_master_signal_master_parameter_response();

void send_master_ack_response();
void send_master_nack_response();
void send_master_signal_master_parameter_request();

void handle_signal_master_parameter_ack_response();
void handle_signal_master_parameter_nack_response();
void handle_signal_master_parameter_unrecognized_response();

void handle_connect_slave_request();
int handle_disconnect_slave_request();
void handle_change_name_slave_request();
void handle_start_cycle_slave_request();
void handle_stop_cycle_slave_request();
void handle_unrecognized_request();
void handle_unexpected_no_request();

#endif /* SLAVE_H */
