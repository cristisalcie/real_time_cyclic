#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/shm.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include "slave.h"



static slave_context_t self = { .shmp = NULL };



static int block_signals() {
    sigset_t block_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block_mask, NULL) == -1) {
        fprintf(stderr, "Can't block signals!\n");
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

// Returns assigned index if successful. NO_PID if failed.
static int getAssignedShmsegIdx(pid_t pid) {
    int shmsegIdx = pid % MAX_SLAVES;

    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (self.shmp->slave_shmseg[shmsegIdx].pid == pid) {
            printf("[SLAVE] Got shared mem idx %d for process %d\n", shmsegIdx, pid);
            return shmsegIdx;
        }
        ++shmsegIdx;
        shmsegIdx %= MAX_SLAVES;
    }

    return NO_IDX;
}

static void calculate_parameter_communication_interval() {
    struct timeval current_timeval;
    gettimeofday(&current_timeval, NULL);

    if (self.last_communication_timeval.tv_sec == 0 && self.last_communication_timeval.tv_usec == 0) {
        self.last_communication_timeval.tv_sec = current_timeval.tv_sec;
        self.last_communication_timeval.tv_usec = current_timeval.tv_usec;
        return;
    }

    long int elapsed_seconds_in_us = (current_timeval.tv_sec - self.last_communication_timeval.tv_sec) * 1000000l;
    long int elapsed_microseconds = elapsed_seconds_in_us + current_timeval.tv_usec - self.last_communication_timeval.tv_usec;

    long int communication_cycle_us = elapsed_microseconds - self.shmp->slave_shmseg[self.shmsegIdx].communication_cycle_us;

    self.communication_cycle_interval_ms = communication_cycle_us / 1000.0;

    self.last_communication_timeval.tv_sec = current_timeval.tv_sec;
    self.last_communication_timeval.tv_usec = current_timeval.tv_usec;

    if (self.communication_cycle_interval_ms > COMMUNICATION_CYCLE_THRESHOLD_US / 1000.0) {
        sprintf(self.shmp->slave_shmseg[self.shmsegIdx].error_string,
            "Exceeded communication cycle set time by %lf miliseconds",
            self.communication_cycle_interval_ms);
    }
}

static void *slave_communication_cycle_detached_thread(void *ignore) {
    while (true) {
        if (sem_wait(&self.allow_communication_cycle)) {
            fprintf(stderr, "sem_wait() call failed!\n");
            return NULL;
        }

        if (self.shmp->slave_shmseg[self.shmsegIdx].communication_cycle_us == NOT_SET_ERROR_COMMUNICATION_CYCLE_MS) {
            // TODO (already done in master)
        }

        if (usleep(self.shmp->slave_shmseg[self.shmsegIdx].communication_cycle_us)) {
            fprintf(stderr, "usleep() failed! Communication cycle thread killed!\n");
            break;
        }

        int rand_value = rand();
        if (self.shmp->slave_shmseg[self.shmsegIdx].requested_parameters & STRING_PARAMETER_BIT) {
            char chr_rand = 'A' + rand_value % ('Z' - 'A' + 1) ;
            self.shmp->slave_shmseg[self.shmsegIdx].string_value[0] = chr_rand;
            self.shmp->slave_shmseg[self.shmsegIdx].string_value[1] = chr_rand + 1;
            self.shmp->slave_shmseg[self.shmsegIdx].string_value[2] = chr_rand + 2;
        }
        if (self.shmp->slave_shmseg[self.shmsegIdx].requested_parameters & INT_PARAMETER_BIT) {
            self.shmp->slave_shmseg[self.shmsegIdx].int_value = rand_value;
        }
        if (self.shmp->slave_shmseg[self.shmsegIdx].requested_parameters & BOOL_PARAMETER_BIT) {
            self.shmp->slave_shmseg[self.shmsegIdx].bool_value = rand_value % 2;
        }

        calculate_parameter_communication_interval();

        self.shmp->slave_shmseg[self.shmsegIdx].req_s_to_m = SIGNAL_MASTER_PARAMETER;
        if (kill(self.shmp->master_pid, SIGUSR1)) {
            fprintf(stderr, "Failed to send \"signal master parameter\" signal to master!\n");
        }
    }

    return NULL;
}

static int create_communication_cycle_detached_thread() {
    pthread_t tid;
    pthread_attr_t attr;
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret) {
        fprintf(stderr, "pthread_attr_init() call failed!\n");
        return RTC_ERROR;
    }

    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (ret) {
        fprintf(stderr, "pthread_attr_setdetachstate() call failed!\n");
        return RTC_ERROR;
    }

    ret = pthread_create(&tid, &attr, slave_communication_cycle_detached_thread, NULL);
    if (ret) {
        fprintf(stderr, "pthread_create() call failed!\n");
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

static int destroy_allow_communication_cycle_semaphore() {
    if (sem_destroy(&self.allow_communication_cycle)) {
        fprintf(stderr, "sem_destroy() call failed!\n");
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

static int final() {
    int ret = RTC_SUCCESS;

    ret = destroy_allow_communication_cycle_semaphore();

    // Deattach from shared memory
    if (shmdt(self.shmp)) {
        fprintf(stderr, "shmdt() call failed!\n");
        ret = RTC_ERROR;
    }
    self.shmp = NULL;

    return ret;
}

static int init_allow_communication_cycle_semaphore() {
    if (sem_init(&self.allow_communication_cycle, 0, 0)) {
        fprintf(stderr, "sem_init() call failed!\n");
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

static int init_shared_memory() {
    int shmid;

    // Get or create if not existing shared memory
    shmid = shmget(SHARED_MEMORY_KEY, sizeof(shm_t), 0666);
    if (shmid == -1) {
        fprintf(stderr, "shmget() call failed to get shared memory id!\n");
        perror("shmget() failed!");
        return RTC_ERROR;
    }

    // Attach to shared memory
    self.shmp = shmat(shmid, NULL, 0);
    if (self.shmp == (void*) -1) {
        fprintf(stderr, "shmmat() call failed to attach to shared memory!\n");
        return RTC_ERROR;
    }

    self.shmsegIdx = NO_IDX;

    return RTC_SUCCESS;
}

static int init(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Syntax: %s <slave_name> <available_parameters>\n", argv[0]);
        return RTC_ERROR;
    }

    int ret;

    memset(&self, 0, sizeof(slave_context_t));

    strcpy(self.name, argv[1]);
    if (argv[2][0] == '1')
        self.available_parameters |= STRING_PARAMETER_BIT;
    if (argv[2][1] == '1')
        self.available_parameters |= INT_PARAMETER_BIT;
    if (argv[2][2] == '1')
        self.available_parameters |= BOOL_PARAMETER_BIT;

    if ((ret = block_signals()) != RTC_SUCCESS) return ret;
    if ((ret = init_shared_memory()) != RTC_SUCCESS) return ret;
    if ((ret = init_allow_communication_cycle_semaphore()) != RTC_SUCCESS) return ret;
    if ((ret = create_communication_cycle_detached_thread()) != RTC_SUCCESS) return ret;

    srand(time(NULL));

    return RTC_SUCCESS;
}


static void *signal_handler_thread(void *ignore) {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR1);

    while (true) {
        err = sigwaitinfo(&sig_set, &sig_info);  // Should queue signals of same value (SIGUSR1)
        if (err == -1) {
            fprintf(stderr, "sigwaitinfo() call failed!\n");
        } else {
            if (sig_info.si_pid != self.shmp->master_pid) {
                fprintf(stderr, "Received signal from process %d. Only accepting signals from master process %d, continuing...\n", sig_info.si_pid, self.shmp->master_pid);
                // TODO 0?: Send to master error message
                continue;
            }
            
            if (sig_info.si_signo == SIGUSR1) {
                if (self.shmsegIdx == NO_IDX) {
                    // First time getting shared memory index.
                    self.shmsegIdx = getAssignedShmsegIdx(getpid());
                    if (self.shmsegIdx == NO_IDX) {
                        // Process has not been assigned a shared memory segment yet.
                        // TODO 0?: Send NACK to master with error message
                        fprintf(stderr, "Process %d has no assigned shared memory index, continuing...\n", getpid());
                        continue;
                    }
                }

                switch (self.shmp->slave_shmseg[self.shmsegIdx].res_m_to_s)
                {
                case ACK:
                    // Master processed, allow slave to process and send again
                    if (sem_post(&self.allow_communication_cycle)) {
                        fprintf(stderr, "sem_post() call failed, continuing...\n");
                        continue;
                    }
                    self.shmp->slave_shmseg[self.shmsegIdx].req_s_to_m = NO_REQUEST;
                    self.shmp->slave_shmseg[self.shmsegIdx].res_m_to_s = NO_RESPONSE;
                    continue;
                case NACK:
                    // Master processed, allow slave to process and try again
                    if (sem_post(&self.allow_communication_cycle)) {
                        fprintf(stderr, "sem_post() call failed, continuing...\n");
                        continue;
                    }
                    self.shmp->slave_shmseg[self.shmsegIdx].req_s_to_m = NO_REQUEST;
                    self.shmp->slave_shmseg[self.shmsegIdx].res_m_to_s = NO_RESPONSE;
                    continue;
                case NO_RESPONSE:
                    // Did not receive a response from master! Continue checking if received request from master.
                    break;
                default:
                    // Master processed, allow slave to process and try again
                    if (sem_post(&self.allow_communication_cycle)) {
                        fprintf(stderr, "sem_post() call failed, continuing...\n");
                        continue;
                    }
                    fprintf(stderr, "Unrecognized response received from master process %d\n", sig_info.si_pid);
                    self.shmp->slave_shmseg[self.shmsegIdx].req_s_to_m = NO_REQUEST;
                    self.shmp->slave_shmseg[self.shmsegIdx].res_m_to_s = NO_RESPONSE;
                    continue;
                }

                // Now check if signals mixed and we also have a request from master, not only response.
                switch (self.shmp->slave_shmseg[self.shmsegIdx].req_m_to_s)
                {
                case CONNECT_SLAVE:
                    handle_connect_slave_request();
                    break;
                case DISCONNECT_SLAVE:
                    handle_disconnect_slave_request();
                    break;
                case CHANGE_SLAVE_NAME:
                    handle_change_name_slave_request();
                    break;
                case START_SLAVE_CYCLE:
                    handle_start_cycle_slave_request();
                    break;
                case STOP_SLAVE_CYCLE:
                    handle_stop_cycle_slave_request();
                    break;
                case NO_REQUEST:
                    fprintf(stderr, "Unexpected NO_REQUEST received from master process %d\n", sig_info.si_pid);
                    break;
                default:
                    fprintf(stderr, "Unrecognized command received from master process %d\n", sig_info.si_pid);
                    // TODO 0: Send NACK to master with error message
                    break;
                }
            }
        }
    }
    return NULL;
}


void handle_connect_slave_request() {
    shmseg_t *shmseg = &self.shmp->slave_shmseg[self.shmsegIdx];

    // Fill shared memory structure
    strcpy(shmseg->name, self.name);
    shmseg->available_parameters = self.available_parameters;

    // Send ACK response back to master
    shmseg->res_s_to_m = ACK;
    if (kill(self.shmp->master_pid, SIGUSR1)) {
        fprintf(stderr, "Failed to send response back to master!\n");
        return;
    }
}

int handle_disconnect_slave_request() {
    // TODO
    return RTC_SUCCESS;
}

void handle_change_name_slave_request() {
    unsigned int prev_change_name_idx = self.change_name_idx;

    if (self.change_name_idx) {
        // Erase previously added number from string name
        for (int i = SLAVE_NAME_SIZE - 1; i > 0; --i) {
            if (self.name[i] == '_') {
                self.name[i] = '\0';
                self.shmp->slave_shmseg[self.shmsegIdx].name[i] = '\0';
                break;
            }
            self.name[i] = '\0';
            self.shmp->slave_shmseg[self.shmsegIdx].name[i] = '\0';
        }
    }
    ++prev_change_name_idx;
    if (prev_change_name_idx < self.change_name_idx) {
        // Index overflow send NACK
        // TODO 0: Set error message for master to log
        self.shmp->slave_shmseg[self.shmsegIdx].res_s_to_m = NACK;
        if (kill(self.shmp->master_pid, SIGUSR1)) {
            fprintf(stderr, "Failed to send response back to master!\n");
        }
        return;
    }
    ++self.change_name_idx;

    sprintf(self.name, "%s_%u", self.shmp->slave_shmseg[self.shmsegIdx].name, self.change_name_idx);
    strcpy(self.shmp->slave_shmseg[self.shmsegIdx].name, self.name);

    printf("Changed slave name to %s\n", self.name);

    self.shmp->slave_shmseg[self.shmsegIdx].res_s_to_m = ACK;
    if (kill(self.shmp->master_pid, SIGUSR1)) {
        fprintf(stderr, "Failed to send response back to master!\n");
    }
}

void handle_start_cycle_slave_request() {
    if (self.cycle_started) {
        // TODO 0: Set error log msg variable for master
        self.shmp->slave_shmseg[self.shmsegIdx].res_s_to_m = NACK;
        if (kill(self.shmp->master_pid, SIGUSR1)) {
            fprintf(stderr, "Failed to send response back to master!\n");
        }
        return;
    }

    if (sem_post(&self.allow_communication_cycle)) {
        fprintf(stderr, "sem_post() call failed!\n");

        // TODO 0: Set error log msg variable for master
        self.shmp->slave_shmseg[self.shmsegIdx].res_s_to_m = NACK;
        if (kill(self.shmp->master_pid, SIGUSR1)) {
            fprintf(stderr, "Failed to send response back to master!\n");
        }
        return;
    }

    self.cycle_started = true;

    self.shmp->slave_shmseg[self.shmsegIdx].res_s_to_m = ACK;
    if (kill(self.shmp->master_pid, SIGUSR1)) {
        fprintf(stderr, "Failed to send response back to master!\n");
    }
}

void handle_stop_cycle_slave_request() {
    // TODO 1
}


int main(int argc, char *argv[]) {
    int ret;
    pthread_t tid;

    if ((ret = init(argc, argv)) != RTC_SUCCESS) return ret;

    // Create signal listening thread
    ret = pthread_create(&tid, NULL, signal_handler_thread, NULL);
    if (ret) {
        fprintf(stderr, "pthread_create() to create signal handler thread failed\n");
        return RTC_ERROR;
    }

    // Wait for signal listening thread to end execution
    ret = pthread_join(tid, NULL);
    if (ret < 0) {
        fprintf(stderr, "pthread_join() to join signal handler thread failed\n");
        return RTC_ERROR;
    }

// "Slaves can also send errors along with the data" - so master checks first for errors

    return final();
}
