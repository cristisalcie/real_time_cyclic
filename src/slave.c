#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/shm.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <pthread.h>
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

static void *slave_communication_cycle_detached_thread(void *ignore) {
    while (true) {
        if (sem_wait(&self.allow_communication_cycle)) {
            fprintf(stderr, "sem_wait() call failed!\n");
            return NULL;
        }

        // TODO 1: sem_post at the end to continue looping
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

static void handle_connect_slave_request() {
    shmseg_t *shmseg = &self.shmp->slave_shmseg[self.shmsegIdx];

    // Fill shared memory structure
    strcpy(shmseg->name, self.name);

    // TODO 1: Send available parameters
    // TODO 1: Wait for master/configurator choice
    // TODO 1: Start communication cycle

    // Send ACK response back to master with available parameters
    shmseg->res_s_to_m = ACK;
    if (kill(self.shmp->master_pid, SIGUSR1)) {
        fprintf(stderr, "Failed to send response back to master!\n");
        return;
    }
}

static int handle_disconnect_slave_request() {
    // TODO
    return RTC_SUCCESS;
}

static void handle_change_name_slave_request() {
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
        // TODO: Set error message for master to log
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

static void handle_start_cycle_slave_request() {
    // TODO 1
}

static void handle_stop_cycle_slave_request() {
    // TODO 1
}

static void *signal_handler_thread(void *ignore) {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR1);

    while (true) {
        printf("[SLAVE %s %d] Waiting for signal!\n", self.name, getpid());
        err = sigwaitinfo(&sig_set, &sig_info);
        if (err == -1) {
            fprintf(stderr, "sigwaitinfo() call failed!\n");
        } else {
            if (sig_info.si_pid != self.shmp->master_pid) {
                fprintf(stderr, "Received signal from process %d. Only accepting signals from master process %d, continuing...\n", sig_info.si_pid, self.shmp->master_pid);
                continue;
            }
            
            if (sig_info.si_signo == SIGUSR1) {
                printf("[SLAVE %s %d] Processing signal SIGUSR1 from process %d\n", self.name, getpid(), sig_info.si_pid);

                if (self.shmsegIdx == NO_IDX) {
                    // First time getting shared memory index.
                    self.shmsegIdx = getAssignedShmsegIdx(getpid());
                    if (self.shmsegIdx == NO_IDX) {
                        // Process has not been assigned a shared memory segment yet.
                        // TODO: Send NACK to master with error message
                        fprintf(stderr, "Process %d has no assigned shared memory index, continuing...\n", getpid());
                        continue;
                    }
                }

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
                // TODO 1: Handle master/configurator parameter choice, cycle time (comes with start cycle request)
                case START_SLAVE_CYCLE:
                    handle_start_cycle_slave_request();
                    break;
                case STOP_SLAVE_CYCLE:
                    handle_stop_cycle_slave_request();
                    break;
                default:
                    fprintf(stderr, "Unrecognized command received from master process %d\n", sig_info.si_pid);
                    // TODO: Send NACK to master with error message
                    break;
                }
            }
        }
    }
    return NULL;
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
    if (argc != 2) {
        fprintf(stderr, "Syntax: %s <slave_name>\n", argv[0]);
        return RTC_ERROR;
    }

    int ret;

    memset(&self, 0, sizeof(slave_context_t));

    strcpy(self.name, argv[1]);

    if ((ret = block_signals()) != RTC_SUCCESS) return ret;
    if ((ret = init_shared_memory()) != RTC_SUCCESS) return ret;
    if ((ret = init_allow_communication_cycle_semaphore()) != RTC_SUCCESS) return ret;

    return RTC_SUCCESS;
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

    // TODO 1: A slave will present all the parameters it has at connect time. The master can request one, more or all parameters.
    // TODO 1: communication and interface with master


    return final();
}
