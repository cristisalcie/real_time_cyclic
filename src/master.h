#ifndef MASTER_H
#define MASTER_H

#include <syslog.h>

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


#endif /* MASTER_H */
