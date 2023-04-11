#ifndef SLAVE_H
#define SLAVE_H

#include "common.h"

// Bigger value => increased busy waiting
#define WAIT_MASTER_RESPONSE_TIMEOUT_SEGMENTS 20

#define print_debug(str, args...)                                                                                    \
    do {                                                                                                             \
        fprintf(stdout, "slave[%d]: [DEBUG] " str "(%s, %s, %d)\n", getpid(), ##args, __FILE__, __func__, __LINE__); \
    } while (0)
#define print_error(str, args...)                                                                                    \
    do {                                                                                                             \
        fprintf(stderr, "slave[%d]: [ERROR] " str "(%s, %s, %d)\n", getpid(), ##args, __FILE__, __func__, __LINE__); \
    } while (0)

// Errors do stack!
#define set_error(str, args...)                                                                 \
    do {                                                                                        \
        print_error(str, ##args);                                                               \
        if (self.shmseg->shmseg_error_current_size < SHMSEG_ERROR_SIZE) {                       \
            snprintf(                                                                           \
                self.shmseg->shmseg_error[self.shmseg->shmseg_error_current_size].error_string, \
                STRING_SIZE,                                                                    \
                str, ##args);                                                                   \
            ++self.shmseg->shmseg_error_current_size;                                           \
        } else {                                                                                \
            print_error("Couldn't set error because error buffer is full!");                    \
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
    sem_t allow_communication_cycle;
    shmseg_t *shmseg;
} slave_context_t;

int init_allow_communication_cycle_semaphore();
int init_shared_memory();
int init(int argc, char *argv[]);

int destroy_allow_communication_cycle_semaphore();
int final();

void send_master_ack_response();
void send_master_nack_response();

// Returns elapsed time waiting for master response always >= 0
long int send_master_signal_master_parameter_request();

void handle_signal_master_parameter_ack_response();
void handle_signal_master_parameter_nack_response();
void handle_signal_master_parameter_unrecognized_response();

void handle_connect_slave_request();
void handle_disconnect_slave_request();
void handle_change_name_slave_request();
void handle_start_cycle_slave_request();
void handle_stop_cycle_slave_request();
void handle_unrecognized_request();

void handle_master_request();
void handle_master_response();

#endif /* SLAVE_H */
