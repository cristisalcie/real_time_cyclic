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

static void handle_connect_slave_request() {
    shmseg_t *shmseg = &self.shmp->slave_shmseg[self.shmsegIdx];

    // Fill shared memory structure
    strcpy(shmseg->name, self.name);

    // TODO: Start communication cycle

    // Send ACK response back to master
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
                    // TODO: Send response back to master
                    break;
                case CHANGE_SLAVE_NAME:
                    handle_change_name_slave_request();
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

static int final() {
    // Deattach from shared memory
    if (shmdt(self.shmp)) {
        fprintf(stderr, "shmdt() call failed!");
        return RTC_ERROR;
    }
    self.shmp = NULL;

    return RTC_SUCCESS;
}

static int init_timer_semaphore() {
    if (sem_init(&self.timer_semaphore, 0, 0)) {
        fprintf(stderr, "sem_init() call failed!");
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

int send_first_signal() {
    if (kill(self.shmp->master_pid, SIGUSR1)) {
        fprintf(stderr, "Failed to send signal SIGUSR1 to master process %d\n", self.shmp->master_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Syntax: %s <slave_name>\n", argv[0]);
        return 1;
    }
    memset(&self, 0, sizeof(slave_context_t));

    strcpy(self.name, argv[1]);
    printf("[SLAVE] Process id = %d with name %s\n", getpid(), self.name);

    int ret;
    pthread_t tid;

    if ((ret = block_signals()) != RTC_SUCCESS) return ret;
    if ((ret = init_shared_memory()) != RTC_SUCCESS) return ret;
    if ((ret = init_timer_semaphore()) != RTC_SUCCESS) return ret;

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

    // TODO: A slave will present all the parameters it has at connect time. The master can request one, more or all parameters.
    // TODO: communication and interface with master


    return final();
}
