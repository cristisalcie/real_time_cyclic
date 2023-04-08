#ifndef MASTER_H
#define MASTER_H

#include <syslog.h>
#include "common.h"
#include "control.h"

#define log_info(str, args...)                                                                  \
    do {                                                                                        \
        syslog(LOG_INFO, "[INFO] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__);  \
    } while(0)
#define log_debug(str, args...)                                                                     \
    do {                                                                                            \
        syslog(LOG_DEBUG, "[DEBUG] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__);    \
    } while(0)
#define log_error(str, args...)                                                                 \
    do {                                                                                        \
        syslog(LOG_ERR, "[ERROR] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__);  \
    } while(0)
#define log_critical(str, args...)                                                                  \
    do {                                                                                            \
        syslog(LOG_CRIT, "[CRITICAL] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__);  \
    } while(0)
#define log_warning(str, args...)                                                                       \
    do {                                                                                                \
        syslog(LOG_WARNING, "[WARNING] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__);    \
    } while(0)

typedef struct master_context_s {
    shm_t *shmp;
    control_shm_t *control_shmp;

    // Signal sleeping processor slave threads using semaphore
    sem_t sig_semaphore[MAX_SLAVES];

    pthread_mutex_t lock_async_handler_threads;
} master_context_t;


int init_signal_semaphores();
int init_control_shared_memory();
void init_slave_shmseg_shared_memory(shmseg_t *slave_shmseg);
int init_shared_memory();
int init_async_signal_handler();
int init();

int destroy_signal_semaphores();
int final();

void send_configurator_ack_response();
void send_configurator_nack_response();

void send_slave_ack_response(shmseg_t *slave_shmseg);
void send_slave_nack_response(shmseg_t *slave_shmseg);

int send_change_name_slave_request(int shmsegIdx);
int send_connect_slave_request(shmseg_t *slave_shmseg);
int send_disconnect_slave_request(shmseg_t *slave_shmseg);
int send_start_cycle_slave_request();
int send_stop_cycle_slave_request();

void handle_slave_request_errors(shmseg_t *slave_shmseg);
int handle_configurator_stop_master_request();
int handle_configurator_connect_slave_request();
int handle_configurator_disconnect_slave_request();
int handle_start_cycle_slave_request();
int handle_stop_cycle_slave_request();

#endif /* MASTER_H */
