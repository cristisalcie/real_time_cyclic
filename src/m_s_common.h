#ifndef M_S_COMMON_H
#define M_S_COMMON_H

#define STRING_SIZE 1024
#define SHARED_MEMORY_KEY 0x1234

typedef struct shmseg_s {
    // Fields modifiable only by slaves
    char name[SLAVE_NAME_SIZE];  // Name string can't contain spaces
    char string_value[STRING_SIZE];
    int int_value;
    bool bool_value;
    response_t res_s_to_m;

    // Fields modifiable only by master
    pid_t pid;  // Master assigns shmsegIdx by setting pid of shared memory segment owner
    request_t req_m_to_s;
    long int communication_cycle_ms; // TODO
    bool is_connected;
} shmseg_t;

typedef struct shm_s {
    // Using hashmap logic on slave_shmseg vector by doing slave_pid % MAX_SLAVES
    // while slave_shmseg[slave_pid % MAX_SLAVES].pid != slave_pid
    // index++;
    // index % MAX_SLAVES
    shmseg_t slave_shmseg[MAX_SLAVES];
    pid_t master_pid;
} shm_t;

#endif /* M_S_COMMON_H */
