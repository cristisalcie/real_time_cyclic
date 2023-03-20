#ifndef COMMON_H
#define COMMON_H

#define STRING_SIZE 1024
#define SLAVE_NAME_SIZE 64
#define MAX_SLAVES 32
#define SHARED_MEMORY_KEY 0x1234

#define RTC_ERROR 1
#define RTC_SUCCESS 0

#define bool __uint8_t
#define false 0
#define true 1
#define NO_PID -1

typedef struct shmseg_s {
    // Fields modifiable only by slaves
    char name[SLAVE_NAME_SIZE];
    char string_value[STRING_SIZE];
    int int_value;
    bool bool_value;

    // TODO: *_value could be union to save memory space

    // Fields modifiable only by master (In order to be able to know if slave sent first ever signal to master and act accordingly)
    pid_t pid;
} shmseg_t;

typedef struct shm_s {
    // Using hashmap logic on slave_shmseg vector by doing slave_pid % MAX_SLAVES
    // while slave_shmseg[slave_pid % MAX_SLAVES].pid != slave_pid
    // index++;
    // index % MAX_SLAVES
    shmseg_t slave_shmseg[MAX_SLAVES];
    pid_t master_pid;
} shm_t;

#endif /* COMMON_H */
