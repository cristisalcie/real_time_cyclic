#ifndef M_S_COMMON_H
#define M_S_COMMON_H

#define STRING_SIZE 1024
#define SHARED_MEMORY_KEY 0x1234

typedef enum {
    S_RS_M_NACK,
    S_RS_M_ACK,
    S_RS_M_CMD_SIZE
} res_from_slave_to_master_cmd;

typedef enum {
    M_RQ_S_CONNECT_SLAVE,
    M_RQ_S_DISCONNECT_SLAVE,
    M_RQ_S_CMD_SIZE
} req_from_master_to_slave_cmd;

typedef struct shmseg_s {
    // Fields modifiable only by slaves
    char name[SLAVE_NAME_SIZE];
    char string_value[STRING_SIZE];
    int int_value;
    bool bool_value;
    res_from_slave_to_master_cmd res_s_to_m_cmd;

    // Fields modifiable only by master
    pid_t pid;  // Master assigns shmsegIdx by setting pid of shared memory segment owner
    req_from_master_to_slave_cmd req_m_to_s_cmd;
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
