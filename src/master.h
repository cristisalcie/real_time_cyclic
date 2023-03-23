#ifndef MASTER_H
#define MASTER_H

#include <syslog.h>
#include "common.h"
#include "m_s_common.h"
#include "control.h"

#define log_info(str, args...) \
    do { \
        syslog(LOG_INFO, "[INFO] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__); \
    } while(0)
#define log_debug(str, args...) \
    do { \
        syslog(LOG_DEBUG, "[DEBUG] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__); \
    } while(0)
#define log_error(str, args...) \
    do { \
        syslog(LOG_ERR, "[ERROR] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__); \
    } while(0)
#define log_critical(str, args...) \
    do { \
        syslog(LOG_CRIT, "[CRITICAL] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__); \
    } while(0)
#define log_warning(str, args...) \
    do { \
        syslog(LOG_WARNING, "[WARNING] " str " (%s, %s, %d)", ##args, __FILE__, __func__, __LINE__); \
    } while(0)

#define DEFAULT_COMMUNICATION_CYCLE_MS 50

typedef struct master_context_s {
    shm_t *shmp;
    control_shm_t *control_shmp;

    // Signal sleeping processor slave threads using semaphore
    sem_t sig_semaphore[MAX_SLAVES];
} master_context_t;

#endif /* MASTER_H */
