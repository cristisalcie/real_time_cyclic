#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include <pthread.h>
#include <string.h>
#include <semaphore.h>

#include "master.h"


static master_context_t self = { .shmp = NULL };



// Returns assigned index if successful. NO_PID if failed.
static int assignShmsegIdx(pid_t pid) {
    int shmsegIdx = pid % MAX_SLAVES;

    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (self.shmp->slave_shmseg[shmsegIdx].pid == NO_PID) {
            log_debug("Assigned shared mem idx %d for process %d", shmsegIdx, pid);
            return shmsegIdx;
        }
        ++shmsegIdx;
        shmsegIdx %= MAX_SLAVES;
    }

    log_error("Can't assign shared memory index, out of memory for process %d", pid);
    return NO_PID;
}

// Returns assigned index if successful. NO_PID if failed.
static int getAssignedShmsegIdx(pid_t pid) {
    int shmsegIdx = pid % MAX_SLAVES;

    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (self.shmp->slave_shmseg[shmsegIdx].pid == pid) {
            log_debug("Got shared mem idx %d for process %d", shmsegIdx, pid);
            return shmsegIdx;
        }
        ++shmsegIdx;
        shmsegIdx %= MAX_SLAVES;
    }

    return shmsegIdx;
    return NO_PID;
}

static void *slave_processor_detached_thread(void *data) {
    int shmsegIdx = *(int*)data;

    while (true) {
        log_debug("Slave processor detached thread with tid %ld preparing to wait for semaphore[%d]", pthread_self(), shmsegIdx);
        if (sem_wait(&(self.sig_semaphore[shmsegIdx]))) {
            // TODO: handle failure
            log_error("sem_wait() call failed!");
            return NULL;
        }
        // TODO: handle possible outcomes
    }

    return NULL;
}

static int create_slave_processor_detached_thread(int shmsegIdx) {
    pthread_t tid;
    pthread_attr_t attr;
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret) {
        log_error("pthread_attr_init() call failed!");
        return RTC_ERROR;
    }

    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (ret) {
        log_error("pthread_attr_setdetachstate() call failed!");
        return RTC_ERROR;
    }

    ret = pthread_create(&tid, &attr, slave_processor_detached_thread, (void*)&shmsegIdx);
    if (ret) {
        log_error("pthread_create() call failed!");
        return RTC_ERROR;
    }

    log_debug("Successfully created slave processor detached thread with tid %ld", tid);
    return RTC_SUCCESS;
}

static void *signal_handler_thread(void *ignore) {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR1);

    while (true) {
        log_debug("Waiting for signal!");
        err = sigwaitinfo(&sig_set, &sig_info);
        if (err == -1) {
            log_error("sigwaitinfo() call failed!");
        } else {
            if (sig_info.si_signo == SIGUSR1) {
                log_debug("Processing signal SIGUSR1 from process %d", sig_info.si_pid);

                int shmsegIdx = getAssignedShmsegIdx(sig_info.si_pid);
                if (shmsegIdx == NO_PID) {
                    // Process has not been assigned a shared memory segment yet.
                    log_debug("Process %d has no assigned index, assigning...", sig_info.si_pid);
                    shmsegIdx = assignShmsegIdx(sig_info.si_pid);

                    if (shmsegIdx == NO_PID) {
                        log_error("Failed to handle signal from process %d, continuing...", sig_info.si_pid);
                        continue;
                    }

                    if (create_slave_processor_detached_thread(shmsegIdx) != RTC_SUCCESS) {
                        log_error("Failed to create slave processor detached thread, continuing...");
                        continue;
                    }
                } else {
                    if (sem_post(&(self.sig_semaphore[shmsegIdx]))) {
                        // TODO: handle failure
                        log_error("sem_post() call failed, continuing...");
                        continue;
                    }
                }
            }
        }
    }
    return NULL;
}



static int destroy_signal_semaphores() {
    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (sem_destroy(&(self.sig_semaphore[i]))) {
            log_error("sem_destroy() call failed!");
            return RTC_ERROR;
        }
    }

    return RTC_SUCCESS;
}

static int final() {
    int ret;

    if ((ret = destroy_signal_semaphores()) != RTC_SUCCESS) return ret;

    // Deattach from shared memory
    if (shmdt(self.shmp)) {
        log_error("shmdt() call failed!");
        return RTC_ERROR;
    }
    self.shmp = NULL;

    log_info("Stopped master!");
    closelog();
    return RTC_SUCCESS;
}

static int block_signals() {
    sigset_t block_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block_mask, NULL) == -1) {
        log_error("Can't block signals!");
        return 1;
    }
    return 0;
}

static int init_signal_semaphores() {
    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (sem_init(&(self.sig_semaphore[i]), 0, 0)) {
            log_error("sem_init() call failed!");
            return RTC_ERROR;
        }
    }

    return RTC_SUCCESS;
}

static int init_shared_memory() {
    int shmid;

    // Get or create if not existing shared memory
    log_info("Shared memory key = %d", SHARED_MEMORY_KEY);

    shmid = shmget(SHARED_MEMORY_KEY, sizeof(shm_t), 0666 | IPC_CREAT);
    if (shmid == -1) {
        log_error("shmget() call failed to create shared memory!");
        return RTC_ERROR;
    }
    // Attach to shared memory
    self.shmp = shmat(shmid, NULL, 0);
    if (self.shmp == (void*) -1) {
        log_error("shmmat() call failed to attach to shared memory!");
        return RTC_ERROR;
    }

#if 0 // if 1, no other process will be able to get shmid of SHARED_MEMORY_KEY
    if (shmctl(shmid, IPC_RMID, 0)) {
        // Once all processes deattached, shared memory will be freed
        log_error("shmctl() call failed!");
        return RTC_ERROR;
    }
#endif

    memset(self.shmp, 0, sizeof(shm_t));

    self.shmp->master_pid = getpid();
    log_debug("Set self.shmp->master_pid to %d", self.shmp->master_pid);

    for (int i = 0; i < MAX_SLAVES; ++i) {
        self.shmp->slave_shmseg[i].pid = NO_PID;
    }

    return RTC_SUCCESS;
}

static int init() {
    int ret;

    openlog("master", LOG_PID | LOG_PERROR, LOG_USER);
    setlogmask(LOG_UPTO(LOG_DEBUG));  // includes all logs 0,1,2 up to 7(DEBUG)
    // setlogmask(LOG_MASK(LOG_INFO));
    log_info("Started master!");

    if (block_signals()) return RTC_ERROR;
    if ((ret = init_shared_memory()) != RTC_SUCCESS) return ret;
    if ((ret = init_signal_semaphores()) != RTC_SUCCESS) return ret;

    return RTC_SUCCESS;
}

int main(int argc, char *argv[]) {
    init(); // TODO: Failed initialization => end program

    int ret;
    pthread_t tid;

    // Create signal listening thread
    ret = pthread_create(&tid, NULL, signal_handler_thread, NULL);
    if (ret) {
        log_error("pthread_create() to create signal handler thread failed");
        return RTC_ERROR;
    }

    // Wait for signal listening thread to end execution
    ret = pthread_join(tid, NULL);                        
    if (ret < 0) {
        log_error("pthread_join() to join signal handler thread failed");
        return RTC_ERROR;
    }



// USE SEMAPHORE

    // signal thread -> slave mutex + lock mutex + create slave thread + send ack back to slave
    // slave thread tries to take the lock mutex inside while(1) but cant
    // slave process sends sigusr1
    // signal thread gets signal and unlocks mutex for slave thread
    // slave thread got lock -> process request -> finally mutex already locked return to while condition trying to take lock again
    // problem - what if slave thread because of error sends another x2 sigusr1 and corresponding slave thread of master did not finish first execution hence unlocks mutex and then it finishes first execution and unlocks mutex again...

    // signal thread -> create slave semaphore with 0 (resources) + send ack back to slave
    // slave thread tries to take a resource from semaphore inside while(1) but cant cuz its 0
    // signal thread receives sigusr1 from slave and adds 1 resource to semaphore of the corresponding slave thread
    // slave thread can now take 1 resource and execute -> finished exec back to condition and there is no resource.

    return final();
}
