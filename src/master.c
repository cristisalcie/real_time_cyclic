#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include <pthread.h>
#include <string.h>
#include <semaphore.h>

#include "master.h"



static master_context_t self;



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
    return NO_IDX;
}

// Use async signal safe functions. Returns assigned index if successful. NO_IDX if failed.
static int getAssignedShmsegIdx(pid_t pid) {
    int shmsegIdx = pid % MAX_SLAVES;

    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (self.shmp->slave_shmseg[shmsegIdx].pid == pid) {
            // log_debug("Got shared mem idx %d for process %d", shmsegIdx, pid);
            return shmsegIdx;
        }
        ++shmsegIdx;
        shmsegIdx %= MAX_SLAVES;
    }

    return NO_IDX;
}

static int block_signals() {
    sigset_t block_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);
    sigaddset(&block_mask, SIGUSR2);
    if (pthread_sigmask(SIG_BLOCK, &block_mask, NULL) == -1) {
        log_error("Can't block signals!");
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

static bool must_change_name(int shmsegIdx) {
    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (i == shmsegIdx || self.shmp->slave_shmseg[i].pid == NO_PID) continue;

        log_debug("to_change_name = %s ; other_names %s", self.shmp->slave_shmseg[shmsegIdx].name, self.shmp->slave_shmseg[i].name);
        if (!strcmp(self.shmp->slave_shmseg[shmsegIdx].name, self.shmp->slave_shmseg[i].name)) {
            return true;
        }
    }
    return false;
}

static bool slave_request_has_errors(shmseg_t *slave_shmseg) {
    return slave_shmseg->shmseg_error_current_size;
}

static void reset_slave_shmseg(shmseg_t *slave_shmseg) {
    log_debug("Resetting slave shmseg that was for pid %d", slave_shmseg->pid);
    memset(slave_shmseg, 0, sizeof(shmseg_t));
    init_slave_shmseg_shared_memory(slave_shmseg);

    // Reset request semaphores back to value 1.
    while (!sem_trywait(&slave_shmseg->sem_m_to_s_request));
    if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
        log_error("sem_post() call failed!");
    }
    while (!sem_trywait(&slave_shmseg->sem_s_to_m_request));
    if (sem_post(&slave_shmseg->sem_s_to_m_request)) {
        log_error("sem_post() call failed!");
    }
}

static void *slave_processor_detached_thread(void *data) {
    int ret;
    int slave_pid = *(int*)data;
    int shmsegIdx = getAssignedShmsegIdx(slave_pid);

    if (shmsegIdx == NO_IDX) {
        log_debug("Process %d has no assigned index (expected), assigning...", slave_pid);
        shmsegIdx = assignShmsegIdx(slave_pid);
        if (shmsegIdx == NO_IDX) {
            log_error("Failed to create shared memory index for process %d!", slave_pid);
            return NULL;
        }
        // Set at shared memory index, pid of process owner
        self.shmp->slave_shmseg[shmsegIdx].pid = slave_pid;
    } else {
        log_error("Slave process %d already connected!", slave_pid);
        return NULL;
    }

    shmseg_t *slave_shmseg = &self.shmp->slave_shmseg[shmsegIdx];

    /* Surely this is first request since configurator can't send any other requests to master
     * that send requests to slave unless master is connected to slave.
     */
    if ((ret = send_connect_slave_request(slave_shmseg)) != RTC_SUCCESS) return NULL;

    while (true) {
        log_debug("Slave processor detached thread with tid %ld preparing to wait for semaphore[%d]", pthread_self(), shmsegIdx);
        if (sem_wait(&(self.sig_semaphore[shmsegIdx]))) {
            // TODO: handle failure
            log_error("sem_wait() call failed!");
            return NULL;
        }

        if (slave_request_has_errors(slave_shmseg)) {
            handle_slave_request_errors(slave_shmseg);
        }

        // TODO: sem_m_to_s_request_val currently could suffer from starvation.
        // Possible solution:
        // Add a variable to change order of semaphore checking.

        int sem_s_to_m_request_val = 1;
        if (sem_getvalue(&slave_shmseg->sem_s_to_m_request, &sem_s_to_m_request_val)) {
            log_error("sem_getvalue() call failed!");
            return NULL;
        }

        if (sem_s_to_m_request_val <= 0) {
            // Handle request, send response, continue to next signal
            handle_slave_request(slave_shmseg);
        } else {
            int sem_m_to_s_request_val = 1;
            if (sem_getvalue(&slave_shmseg->sem_m_to_s_request, &sem_m_to_s_request_val)) {
                log_error("sem_getvalue() call failed!");
                return NULL;
            }

            if (sem_m_to_s_request_val <= 0) {
                // Receive response signal, increment semaphore
                if (handle_slave_response(shmsegIdx) != RTC_SUCCESS) return NULL;
            } else {
                // No request towards slave unhandled (warn/error)
                log_error("No request towards slave unhandled!");
            }
        }
    }

    return NULL;
}

static int create_slave_processor_detached_thread(int slave_pid) {
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

    ret = pthread_create(&tid, &attr, slave_processor_detached_thread, (void*)&slave_pid);
    if (ret) {
        log_error("pthread_create() call failed!");
        return RTC_ERROR;
    }

    log_debug("Successfully created slave processor detached thread with tid %ld", tid);
    return RTC_SUCCESS;
}

static void async_signal_handler(int signum, siginfo_t *sig_info, void *context) {
    switch (signum)
    {
    case SIGUSR1:
        {
            int shmsegIdx = getAssignedShmsegIdx(sig_info->si_pid);

            if (shmsegIdx == NO_IDX) {
                // TODO: log_error("Received signal SIGUSR1 from unrecognized process %d, continuing...", sig_info->si_pid);
            } else {
                // Allow thread allocated for pid to process
                if (sem_post(&(self.sig_semaphore[shmsegIdx]))) {
                    // TODO: log_error("sem_post() call failed, continuing...");
                }
            }
            break;
        }
    }
}

int init_signal_semaphores() {
    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (sem_init(&self.shmp->slave_shmseg[i].sem_s_to_m_request, 1, 1)) {
            log_error("sem_init() call failed!");
            return RTC_ERROR;
        }
        if (sem_init(&self.shmp->slave_shmseg[i].sem_m_to_s_request, 1, 1)) {
            log_error("sem_init() call failed!");
            return RTC_ERROR;
        }
        if (sem_init(&self.sig_semaphore[i], 0, 0)) {
            log_error("sem_init() call failed!");
            return RTC_ERROR;
        }
    }

    return RTC_SUCCESS;
}

int init_control_shared_memory() {
    int shmid;

    // Get existing shared memory
    shmid = shmget(CONTROL_SHARED_MEMORY_KEY, sizeof(control_shm_t), 0666);
    if (shmid == -1) {
        log_error("shmget() call failed to create shared memory!\n");
        return RTC_ERROR;
    }
    // Attach to shared memory
    self.control_shmp = shmat(shmid, NULL, 0);
    if (self.control_shmp == (void*) -1) {
        log_error("shmmat() call failed to attach to shared memory!\n");
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

void init_slave_shmseg_shared_memory(shmseg_t *slave_shmseg) {
    slave_shmseg->pid = NO_PID;
    slave_shmseg->communication_cycle_us = ERROR_NOT_SET_COMMUNICATION_CYCLE_MS;

    for (int j = 0; j < SHMSEG_ERROR_SIZE; ++j) {
        memset(slave_shmseg->shmseg_error[j].error_string, STRING_VALUE_UNDEFINED, STRING_SIZE);
    }

    memset(slave_shmseg->string_value, STRING_VALUE_UNDEFINED, STRING_SIZE);
    memset(slave_shmseg->last_known_string_value, STRING_VALUE_UNDEFINED, STRING_SIZE);
    slave_shmseg->int_value = INT_VALUE_UNDEFINED;
    slave_shmseg->last_known_int_value = INT_VALUE_UNDEFINED;
    slave_shmseg->bool_value = BOOL_VALUE_UNDEFINED;
    slave_shmseg->last_known_bool_value = BOOL_VALUE_UNDEFINED;
}

int init_shared_memory() {
    int shmid;

    // Get or create if not existing shared memory
    log_debug("Shared memory key = %d", SHARED_MEMORY_KEY);

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

    memset(self.shmp, 0, sizeof(shm_t));

    self.shmp->master_pid = getpid();
    log_debug("Set self.shmp->master_pid to %d", self.shmp->master_pid);

    for (int i = 0; i < MAX_SLAVES; ++i) {
        init_slave_shmseg_shared_memory(&self.shmp->slave_shmseg[i]);
    }

    return RTC_SUCCESS;
}

int init_async_signal_handler() {
    struct sigaction sact;

    // Only allow SIGUSR1 to interrupt handler execution
    sigfillset(&sact.sa_mask);
    sigdelset(&sact.sa_mask, SIGUSR1);
    sact.sa_flags = SA_SIGINFO | SA_NODEFER;
    sact.sa_sigaction = async_signal_handler;

    // Register SIGUSR1 to signal_handler with sact options
    if (sigaction(SIGUSR1, &sact, NULL) != 0) {
        log_error("sigaction() failed!");
        return RTC_ERROR;
    }

    if (pthread_mutex_init(&self.lock_async_handler_threads, NULL)) {
        log_error("pthread_mutex_init() failed!");
        return RTC_ERROR;
    }

    if (pthread_mutex_lock(&self.lock_async_handler_threads)) {
        log_error("pthread_mutex_lock() failed!");
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

int init() {
    memset(&self, 0, sizeof(master_context_t));
    
    int ret;

    // openlog("master", LOG_PID | LOG_PERROR, LOG_USER);
    openlog("master", LOG_PID, LOG_USER);
    setlogmask(LOG_UPTO(LOG_DEBUG));  // includes all logs 0,1,2 up to 7(DEBUG)
    // setlogmask(LOG_MASK(LOG_ERR) | LOG_MASK(LOG_INFO));
    log_info("Started master!");

    if ((ret = block_signals()) != RTC_SUCCESS) return ret;
    if ((ret = init_shared_memory()) != RTC_SUCCESS) return ret;
    if ((ret = init_control_shared_memory()) != RTC_SUCCESS) return ret;
    if ((ret = init_signal_semaphores()) != RTC_SUCCESS) return ret;
    if ((ret = init_async_signal_handler()) != RTC_SUCCESS) return ret;

    return RTC_SUCCESS;
}


int destroy_signal_semaphores() {
    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (sem_destroy(&self.sig_semaphore[i])) {
            log_error("sem_destroy() call failed!");
            return RTC_ERROR;
        }
        if (sem_destroy(&self.shmp->slave_shmseg[i].sem_s_to_m_request)) {
            log_error("sem_destroy() call failed!");
            return RTC_ERROR;
        }
        if (sem_destroy(&self.shmp->slave_shmseg[i].sem_m_to_s_request)) {
            log_error("sem_destroy() call failed!");
            return RTC_ERROR;
        }
    }

    return RTC_SUCCESS;
}

int final() {
    int ret = RTC_SUCCESS;

    ret |= pthread_mutex_unlock(&self.lock_async_handler_threads);

    ret |= destroy_signal_semaphores();
    ret |= pthread_mutex_destroy(&self.lock_async_handler_threads);

    // Deattach from shared memory
    if (shmdt(self.shmp)) {
        log_error("shmdt() call failed!");
        ret = RTC_ERROR;
    }
    self.shmp = NULL;

    log_info("Stopped master!");
    closelog();
    return ret;
}


// Purpose of this function is to have a thread be able to receive unblocked signals to call handlers async
static void *async_signal_handler_thread(void *ignore) {
    sigset_t unblock_mask;
    sigemptyset(&unblock_mask);
    sigaddset(&unblock_mask, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &unblock_mask, NULL) == -1) {
        log_error("Failed to unblock SIGUSR1");
        return NULL;
    }

    // Block this thread into a mutex
    if (pthread_mutex_lock(&self.lock_async_handler_threads)) {
        log_error("Failed to keep alive async_signal_handler_thread()");
    }

    return NULL;
}

static void *sync_signal_handler_thread(void *ignore) {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR2);

    while (true) {
        log_debug("[%ld] Waiting for signal!", pthread_self());
        err = sigwaitinfo(&sig_set, &sig_info);
        if (err == -1) {
            log_error("[%ld] sigwaitinfo() call failed!", pthread_self());
        } else {
            log_debug("[%ld] Processing signal %d from process %d", pthread_self(), sig_info.si_signo, sig_info.si_pid);

            if (sig_info.si_signo == SIGUSR2) {
                switch (self.control_shmp->request) {
                case STOP_MASTER:
                    handle_configurator_stop_master_request();
                    break;
                case CONNECT_SLAVE:
                    if (handle_configurator_connect_slave_request() != RTC_SUCCESS) {
                        // ACK response is send to configurator when ACK response received from slave
                        send_configurator_nack_response();
                    }
                    break;
                case DISCONNECT_SLAVE:
                    if (handle_configurator_disconnect_slave_request() != RTC_SUCCESS) {
                        // ACK response is send to configurator when ACK response received from slave
                        send_configurator_nack_response();
                    }
                    break;
                case START_SLAVE_CYCLE:
                    if (handle_configurator_start_cycle_slave_request() != RTC_SUCCESS) {
                        // ACK response is send to configurator when ACK response received from slave
                        send_configurator_nack_response();
                    }
                    break;
                case STOP_SLAVE_CYCLE:
                    if (handle_configurator_stop_cycle_slave_request() != RTC_SUCCESS) {
                        // ACK response is send to configurator when ACK response received from slave
                        send_configurator_nack_response();
                    }
                    break;
                default:
                    log_error("Received unknown command(%d) from configurator process %d", self.control_shmp->request, self.control_shmp->configurator_pid);
                    break;
                }
            }
        }
    }
    return NULL;
}


void send_configurator_ack_response() {
    self.control_shmp->response = ACK;
    if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
        log_error("Failed to send NACK signal to configurator!");
    }
}

void send_configurator_nack_response() {
    self.control_shmp->response = NACK;
    if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
        log_error("Failed to send NACK signal to configurator!");
    }
}

void send_slave_ack_response(shmseg_t *slave_shmseg) {
    slave_shmseg->res_m_to_s = ACK;
    if (kill(slave_shmseg->pid, SIGUSR1)) {
        log_error("Failed to send ACK signal for SIGNAL_MASTER_PARAMETER to slave process %d!", slave_shmseg->pid);
    }
}

void send_slave_nack_response(shmseg_t *slave_shmseg) {
    slave_shmseg->res_m_to_s = NACK;
    if (kill(slave_shmseg->pid, SIGUSR1)) {
        log_error("Failed to send ACK signal for SIGNAL_MASTER_PARAMETER to slave process %d!", slave_shmseg->pid);
    }
}

int send_change_name_slave_request(shmseg_t *slave_shmseg) {
    pid_t slave_pid = slave_shmseg->pid;

    log_debug("called send_change_name_slave_request() to process %d, waiting semaphore", slave_shmseg->pid);

    if (sem_wait(&slave_shmseg->sem_m_to_s_request)) {
        log_error("sem_wait() call failed!");
        return RTC_ERROR;
    }

    log_debug("called send_change_name_slave_request() to process %d, got semaphore", slave_shmseg->pid);

    slave_shmseg->req_m_to_s = CHANGE_SLAVE_NAME;

    if (kill(slave_pid, SIGUSR1)) {
        log_error("Failed to send change name slave request to process %d!", slave_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_connect_slave_request(shmseg_t *slave_shmseg) {
    log_debug("called send_connect_slave_request() to process %d, waiting semaphore", slave_shmseg->pid);

    if (sem_wait(&slave_shmseg->sem_m_to_s_request)) {
        log_error("sem_wait() call failed!");
        return RTC_ERROR;
    }

    log_debug("called send_connect_slave_request() to process %d, got semaphore", slave_shmseg->pid);

    slave_shmseg->req_m_to_s = CONNECT_SLAVE;

    if (kill(slave_shmseg->pid, SIGUSR1)) {
        log_error("Failed to send connect slave request to process %d!", slave_shmseg->pid);
        reset_slave_shmseg(slave_shmseg);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_disconnect_slave_request(shmseg_t *slave_shmseg) {

    log_debug("called send_disconnect_slave_request() to process %d, waiting semaphore", slave_shmseg->pid);

    if (sem_wait(&slave_shmseg->sem_m_to_s_request)) {
        log_error("sem_wait() call failed!");
        return RTC_ERROR;
    }

    log_debug("called send_disconnect_slave_request() to process %d, got semaphore", slave_shmseg->pid);

    slave_shmseg->req_m_to_s = DISCONNECT_SLAVE;

    if (kill(slave_shmseg->pid, SIGUSR1)) {
        log_error("Failed to send disconnect slave request to process %d!", slave_shmseg->pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_start_cycle_slave_request() {
    pid_t slave_pid = self.control_shmp->affected_slave_pid;

    int shmsegIdx = getAssignedShmsegIdx(slave_pid);
    if (shmsegIdx == NO_IDX) {
        log_debug("Process %d has no assigned index!", slave_pid);
        return RTC_ERROR;
    }

    shmseg_t *slave_shmseg = &self.shmp->slave_shmseg[shmsegIdx];

    if (slave_shmseg->communication_cycle_us == ERROR_NOT_SET_COMMUNICATION_CYCLE_MS) {
        log_error("Process %d has not been set communication cycle variable!", slave_pid);
        return RTC_ERROR;
    }

    log_debug("called send_start_cycle_slave_request() to process %d, waiting semaphore", slave_shmseg->pid);

    if (sem_wait(&slave_shmseg->sem_m_to_s_request)) {
        log_error("sem_wait() call failed!");
        return RTC_ERROR;
    }

    log_debug("called send_start_cycle_slave_request() to process %d, got semaphore", slave_shmseg->pid);

    slave_shmseg->req_m_to_s = START_SLAVE_CYCLE;

    if (kill(slave_pid, SIGUSR1)) {
        log_error("Failed to send start cycle slave request to process %d!", slave_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_stop_cycle_slave_request() {
    pid_t slave_pid = self.control_shmp->affected_slave_pid;

    int shmsegIdx = getAssignedShmsegIdx(slave_pid);
    if (shmsegIdx == NO_IDX) {
        log_debug("Process %d has no assigned index!", slave_pid);
        return RTC_ERROR;
    }

    shmseg_t *slave_shmseg = &self.shmp->slave_shmseg[shmsegIdx];

    log_debug("called send_stop_cycle_slave_request() to process %d, waiting semaphore", slave_shmseg->pid);

    if (sem_wait(&slave_shmseg->sem_m_to_s_request)) {
        log_error("sem_wait() call failed!");
        return RTC_ERROR;
    }

    log_debug("called send_stop_cycle_slave_request() to process %d, got semaphore", slave_shmseg->pid);

    slave_shmseg->req_m_to_s = STOP_SLAVE_CYCLE;

    if (kill(slave_pid, SIGUSR1)) {
        log_error("Failed to send stop cycle slave request to process %d!", slave_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}


void handle_slave_request_errors(shmseg_t *slave_shmseg) {
    for (int i = 0; i < slave_shmseg->shmseg_error_current_size; ++i) {
        log_error("Received from slave process [%d]: %s",
            slave_shmseg->pid,
            slave_shmseg->shmseg_error[i].error_string);
        memset(slave_shmseg->shmseg_error[i].error_string, STRING_VALUE_UNDEFINED, STRING_SIZE);
    }
    slave_shmseg->shmseg_error_current_size = 0;
}

int handle_configurator_stop_master_request() {
    // TODO: Improve stop master request
    return RTC_SUCCESS;
}

int handle_configurator_connect_slave_request() {
    // Create slave process thread that will handle slave.
    if (create_slave_processor_detached_thread(self.control_shmp->affected_slave_pid) != RTC_SUCCESS) {
        log_error("Failed to create slave processor detached thread, continuing...");
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

int handle_configurator_disconnect_slave_request() {
    int shmsegIdx = getAssignedShmsegIdx(self.control_shmp->affected_slave_pid);
    return send_disconnect_slave_request(&self.shmp->slave_shmseg[shmsegIdx]);
}

int handle_configurator_start_cycle_slave_request() {
    return send_start_cycle_slave_request();
}

int handle_configurator_stop_cycle_slave_request() {
    return send_stop_cycle_slave_request();
}

void handle_slave_request(shmseg_t *slave_shmseg) {
    if (!slave_shmseg->is_connected) return;

    switch (slave_shmseg->req_s_to_m) {
    case SIGNAL_MASTER_PARAMETER:
        if (slave_shmseg->requested_parameters & STRING_PARAMETER_BIT) {
            if (slave_shmseg->string_value[0] == STRING_VALUE_UNDEFINED) {
                log_error("Did not receive requested string parameter from slave process [%d]!",
                    slave_shmseg->pid);
            } else {
                log_debug("Received from slave process [%d] string parameter %s",
                    slave_shmseg->pid,
                    slave_shmseg->string_value);
                strcpy(slave_shmseg->last_known_string_value, slave_shmseg->string_value);
                memset(slave_shmseg->string_value, STRING_VALUE_UNDEFINED, STRING_SIZE);
            }
        }
        if (slave_shmseg->requested_parameters & INT_PARAMETER_BIT) {
            if (slave_shmseg->int_value == INT_VALUE_UNDEFINED) {
                log_error("Did not receive requested int parameter from slave process [%d]!",
                    slave_shmseg->pid);
            } else {
                log_debug("Received from slave process [%d] int parameter %d",
                    slave_shmseg->pid, slave_shmseg->int_value);
                slave_shmseg->last_known_int_value = slave_shmseg->int_value;
                slave_shmseg->int_value = INT_VALUE_UNDEFINED;
            }
        }
        if (slave_shmseg->requested_parameters & BOOL_PARAMETER_BIT) {
            if (slave_shmseg->bool_value == BOOL_VALUE_UNDEFINED) {
                log_error("Did not receive requested bool parameter from slave process [%d]!",
                    slave_shmseg->pid);
            } else {
                log_debug("Received from slave process [%d] bool parameter %s",
                    slave_shmseg->pid,
                    slave_shmseg->bool_value ? "true" : "false");
                slave_shmseg->last_known_bool_value = slave_shmseg->bool_value;
                slave_shmseg->bool_value = BOOL_VALUE_UNDEFINED;
            }
        }

        send_slave_ack_response(slave_shmseg);

        break;
    default:
        log_error("Unrecognized request from slave process %d!", slave_shmseg->pid);
        break;
    }
}

int handle_slave_response(int shmsegIdx) {
    shmseg_t *slave_shmseg = &self.shmp->slave_shmseg[shmsegIdx];

    if (slave_shmseg->is_connected) {
        switch (slave_shmseg->req_m_to_s) {
        case DISCONNECT_SLAVE:
            log_debug("Handling DISCONNECT_SLAVE slave response!");
            switch (slave_shmseg->res_s_to_m) {
            case NACK:
                {
                    log_error("Failed to disconnect slave!");
                    send_configurator_nack_response();
                }
                break;
            case ACK:
                {
                    reset_slave_shmseg(slave_shmseg);
                    send_configurator_ack_response();
                    return RTC_ERROR;  // Stop detached thread
                }
                break;
            default:
                {
                    log_error("Unrecognized response received to master disconnect slave request!");
                }
                break;
            }
            break;
        case STOP_SLAVE_CYCLE:
        case START_SLAVE_CYCLE:
            log_debug("Handling START_SLAVE_CYCLE/STOP_SLAVE_CYCLE slave response!");
            switch (slave_shmseg->res_s_to_m) {
            case NACK:
                {
                    log_debug("Failed to start/stop slave cycle!");
                    // Forward response to configurator
                    send_configurator_nack_response();
                }
                break;
            case ACK:
                {
                    // Forward response to configurator
                    send_configurator_ack_response();
                }
                break;
            default:
                {
                    log_error("Unrecognized response for start/stop slave cycle request received!");
                    send_configurator_nack_response();
                }
                break;
            }
            break;
        default:
            {
                log_error("Received response for unrecognized request!");
            }
            break;
        }
        if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
            log_error("sem_post() call failed!");
        }
    } else {
        // Has send_requests functions inside. Must increment request semaphore carefully!
        switch (slave_shmseg->req_m_to_s) {
        case CONNECT_SLAVE:
            log_debug("Handling CONNECT_SLAVE slave response!");
            switch (slave_shmseg->res_s_to_m) {
            case NACK:
                {
                    send_configurator_nack_response();
                    if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                        log_error("sem_post() call failed!");
                    }
                }
                break;
            case ACK:
                {
                    if (must_change_name(shmsegIdx)) {
                        if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                            log_error("sem_post() call failed!");
                        }
                        /* Before sending request make sure we finish previous
                         * one by incrementing corresponding semaphore.
                         */
                        send_change_name_slave_request(slave_shmseg);
                    } else {
                        slave_shmseg->is_connected = true;
                        send_configurator_ack_response();
                        if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                            log_error("sem_post() call failed!");
                        }
                    }
                }
                break;
            default:
                {
                    log_error("Unrecognized command from process %d, continuing...", slave_shmseg->pid);
                    send_configurator_nack_response();
                    if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                        log_error("sem_post() call failed!");
                    }
                }
                break;
            }
            break;
        case CHANGE_SLAVE_NAME:
            log_debug("Handling CHANGE_SLAVE_NAME slave response!");
            switch (slave_shmseg->res_s_to_m) {
            case NACK:
                {
                    // TODO: Disconnect slave? This case should not happen
                    send_configurator_nack_response();
                    if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                        log_error("sem_post() call failed!");
                    }
                }
                break;
            case ACK:
                {
                    if (must_change_name(shmsegIdx)) {
                        if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                            log_error("sem_post() call failed!");
                        }
                        send_change_name_slave_request(slave_shmseg);
                    } else {
                        slave_shmseg->is_connected = true;

                        send_configurator_ack_response();
                        if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                            log_error("sem_post() call failed!");
                        }
                    }
                }
                break;
            default:
                {
                    log_error("Unrecognized response for name change request received!");
                    send_configurator_nack_response();
                    if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                        log_error("sem_post() call failed!");
                    }
                }
                break;
            }
            break;
        default:
            {
                log_error("Unrecognized request received for response for process %d", slave_shmseg->pid);
                if (sem_post(&slave_shmseg->sem_m_to_s_request)) {
                    log_error("sem_post() call failed!");
                }
            }
            break;
        }
    }
    return RTC_SUCCESS;
}

int main(int argc, char *argv[]) {
    init(); // TODO: Failed initialization => end program

    int ret;
    pthread_t tid1;
    pthread_t tid2;

    // Create signal listening threads
    ret = pthread_create(&tid1, NULL, async_signal_handler_thread, NULL);
    if (ret) {
        log_error("pthread_create() to create signal handler thread failed");
        return RTC_ERROR;
    }
    ret = pthread_create(&tid2, NULL, sync_signal_handler_thread, NULL);
    if (ret) {
        log_error("pthread_create() to create signal handler thread failed");
        return RTC_ERROR;
    }

    // Full initialization complete, send signal to configurator
    if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
        log_error("Failed to send initialization complete response to configurator!");
        return RTC_ERROR;
    }

    // Wait for signal listening thread to end execution
    ret = pthread_join(tid1, NULL);                        
    if (ret < 0) {
        log_error("pthread_join() to join signal handler thread failed");
        return RTC_ERROR;
    }
    ret = pthread_join(tid2, NULL);                        
    if (ret < 0) {
        log_error("pthread_join() to join signal handler thread failed");
        return RTC_ERROR;
    }

    return final();
}
