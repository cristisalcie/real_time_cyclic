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
    return NO_IDX;
}

// Returns assigned index if successful. NO_IDX if failed.
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
    if (sigprocmask(SIG_BLOCK, &block_mask, NULL) == -1) {
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
    if (slave_shmseg->error_string[0]) {
        return true;
    }
    return false;
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

    if ((ret = send_connect_slave_request(slave_shmseg)) != RTC_SUCCESS) return NULL;

    while (true) {
        log_debug("Slave processor detached thread with tid %ld preparing to wait for semaphore[%d]", pthread_self(), shmsegIdx);
        if (sem_wait(&(self.sig_semaphore[shmsegIdx]))) {
            // TODO: handle failure
            log_error("sem_wait() call failed!");
            return NULL;
        }

        if (slave_shmseg->is_connected) {
            switch (slave_shmseg->req_s_to_m)
            {
            case SIGNAL_MASTER_PARAMETER:
                if (slave_request_has_errors(slave_shmseg)) {
                    handle_slave_request_errors(slave_shmseg);
                }

                if (slave_shmseg->requested_parameters & STRING_PARAMETER_BIT) {
                    if (slave_shmseg->string_value[0] == STRING_VALUE_UNDEFINED) {
                        log_error("Did not receive requested string parameter from slave process [%d]!",
                            slave_shmseg->pid);
                    } else {
                        log_info("Received from slave process [%d] string parameter %s",
                            slave_shmseg->pid,
                            slave_shmseg->string_value);
                        memset(slave_shmseg->string_value, STRING_VALUE_UNDEFINED, STRING_SIZE);
                    }
                }
                if (slave_shmseg->requested_parameters & INT_PARAMETER_BIT) {
                    if (slave_shmseg->int_value == INT_VALUE_UNDEFINED) {
                        log_error("Did not receive requested int parameter from slave process [%d]!",
                            slave_shmseg->pid);
                    } else {
                        log_info("Received from slave process [%d] int parameter %d",
                            slave_shmseg->pid, slave_shmseg->int_value);
                        slave_shmseg->int_value = INT_VALUE_UNDEFINED;
                    }
                }
                if (slave_shmseg->requested_parameters & BOOL_PARAMETER_BIT) {
                    if (slave_shmseg->bool_value == BOOL_VALUE_UNDEFINED) {
                        log_error("Did not receive requested bool parameter from slave process [%d]!",
                            slave_shmseg->pid);
                    } else {
                        log_info("Received from slave process [%d] bool parameter %s",
                            slave_shmseg->pid,
                            slave_shmseg->bool_value ? "true" : "false");
                        slave_shmseg->bool_value = BOOL_VALUE_UNDEFINED;
                    }
                }

                slave_shmseg->res_m_to_s = ACK;
                if (kill(slave_shmseg->pid, SIGUSR1)) {
                    log_error("Failed to send ACK signal for SIGNAL_MASTER_PARAMETER to slave process %d!", slave_shmseg->pid);
                }
                continue;
            case NO_REQUEST:
                // Did not receive a request from slave! Continue checking if received response from slave
                break;
            default:
                log_error("Unrecognized request from slave process %d!", slave_shmseg->pid);
                continue;
            }

            switch (slave_shmseg->req_m_to_s)
            {
            case STOP_SLAVE_CYCLE:
            case START_SLAVE_CYCLE:
                switch (slave_shmseg->res_s_to_m)
                {
                case NACK:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                
                    slave_shmseg->req_m_to_s = NO_REQUEST;
                    log_info("[DEBUG] slave_shmseg->req_m_to_s = NO_REQUEST");

                    // Forward response to configurator
                    self.control_shmp->response = NACK;
                    if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                        log_error("Failed to send NACK signal to configurator!");
                    }

                    break;
                case ACK:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                    log_info("[DEBUG] slave_shmseg->req_m_to_s = NO_REQUEST");
                    slave_shmseg->req_m_to_s = NO_REQUEST;

                    // Forward response to configurator
                    log_debug("SENT RESPONSE TO CONFIGURATOR TO START_SLAVE_CYCLE/STOP_SLAVE_CYCLE ACK RESPONSE");
                    self.control_shmp->response = ACK;
                    if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                        log_error("Failed to send ACK signal to configurator!");
                    }

                    break;
                default:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                    log_info("[DEBUG] slave_shmseg->req_m_to_s = NO_REQUEST");
                    slave_shmseg->req_m_to_s = NO_REQUEST;
                    log_error("Unrecognized response for start slave cycle request received!");
                    break;
                }
                break;

            default:
                slave_shmseg->res_s_to_m = NO_RESPONSE;
                slave_shmseg->req_m_to_s = NO_REQUEST;
                log_error("Received response for unrecognized request!");
                break;
            }
        } else {
            switch (slave_shmseg->req_m_to_s)
            {
            case CONNECT_SLAVE:
                switch (slave_shmseg->res_s_to_m) {
                case NACK:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                    log_debug("slave_shmseg->res_s_to_m = NO_RESPONSE");

                    self.control_shmp->response = NACK;
                    if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                        log_error("Failed to send NACK signal to configurator!");
                    }
                    slave_shmseg->req_m_to_s = NO_REQUEST;
                    log_debug("slave_shmseg->req_m_to_s = NO_REQUEST");
                    break;
                case ACK:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                    log_debug("slave_shmseg->res_s_to_m = NO_RESPONSE");

                    if (must_change_name(shmsegIdx)) {
                        send_change_name_slave_request(shmsegIdx);
                    } else {
                        slave_shmseg->is_connected = true;
                        slave_shmseg->req_m_to_s = NO_REQUEST;
                        log_debug("slave_shmseg->req_m_to_s = NO_REQUEST");
                        self.control_shmp->response = ACK;
                        if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                            log_error("Failed to send ACK signal to configurator!");
                        }
                    }

                    break;
                default:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                    log_debug("slave_shmseg->res_s_to_m = NO_RESPONSE");
                    log_error("Unrecognized command from process %d, continuing...", slave_pid);
                    slave_shmseg->req_m_to_s = NO_REQUEST;
                    log_debug("slave_shmseg->req_m_to_s = NO_REQUEST");
                    self.control_shmp->response = NACK;
                    if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                        log_error("Failed to send NACK signal to configurator!");
                    }
                    break;
                }
                break;

            case CHANGE_SLAVE_NAME:
                switch (slave_shmseg->res_s_to_m)
                {
                case NACK:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                    log_debug("slave_shmseg->res_s_to_m = NO_RESPONSE");

                    slave_shmseg->req_m_to_s = NO_REQUEST;
                    log_debug("slave_shmseg->req_m_to_s = NO_REQUEST");

                    // TODO: Disconnect slave!
                    self.control_shmp->response = NACK;
                    if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                        log_error("Failed to send NACK signal to configurator!");
                    }

                    break;
                case ACK:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                    log_debug("slave_shmseg->res_s_to_m = NO_RESPONSE");

                    if (must_change_name(shmsegIdx)) {
                        send_change_name_slave_request(shmsegIdx);
                    } else {
                        slave_shmseg->is_connected = true;

                        log_debug("slave_shmseg->req_m_to_s = NO_REQUEST");
                        slave_shmseg->req_m_to_s = NO_REQUEST;

                        self.control_shmp->response = ACK;
                        if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                            log_error("Failed to send ACK signal to configurator!");
                        }
                    }
                    break;
                
                default:
                    slave_shmseg->res_s_to_m = NO_RESPONSE;
                    log_debug("slave_shmseg->res_s_to_m = NO_RESPONSE");
                    slave_shmseg->req_m_to_s = NO_REQUEST;
                    log_debug("slave_shmseg->req_m_to_s = NO_REQUEST");
                    log_error("Unrecognized response for name change request received!");
                    break;
                }
                break;
            default:
                log_error("Unrecognized request received for response for process %d", slave_pid);
                break;
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
    int ret = RTC_SUCCESS;

    ret = destroy_signal_semaphores();

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

static int init_signal_semaphores() {
    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (sem_init(&(self.sig_semaphore[i]), 0, 0)) {
            log_error("sem_init() call failed!");
            return RTC_ERROR;
        }
    }

    return RTC_SUCCESS;
}

static int init_control_shared_memory() {
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

static int init_shared_memory() {
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
        self.shmp->slave_shmseg[i].pid = NO_PID;
        self.shmp->slave_shmseg[i].communication_cycle_us = NOT_SET_ERROR_COMMUNICATION_CYCLE_MS;
        self.shmp->slave_shmseg[i].req_m_to_s = NO_REQUEST;
        self.shmp->slave_shmseg[i].req_s_to_m = NO_REQUEST;
        self.shmp->slave_shmseg[i].res_m_to_s = NO_RESPONSE;
        self.shmp->slave_shmseg[i].res_s_to_m = NO_RESPONSE;
        memset(self.shmp->slave_shmseg[i].error_string, STRING_VALUE_UNDEFINED, STRING_SIZE);
        memset(self.shmp->slave_shmseg[i].string_value, STRING_VALUE_UNDEFINED, STRING_SIZE);
        self.shmp->slave_shmseg[i].int_value = INT_VALUE_UNDEFINED;
        self.shmp->slave_shmseg[i].bool_value = BOOL_VALUE_UNDEFINED;
    }

    return RTC_SUCCESS;
}

static int init() {
    int ret;

    openlog("master", LOG_PID | LOG_PERROR, LOG_USER);
    // openlog("master", LOG_PID, LOG_USER);
    setlogmask(LOG_UPTO(LOG_DEBUG));  // includes all logs 0,1,2 up to 7(DEBUG)
    // setlogmask(LOG_MASK(LOG_ERR) | LOG_MASK(LOG_INFO));
    log_info("Started master!");

    if ((ret = block_signals()) != RTC_SUCCESS) return ret;
    if ((ret = init_shared_memory()) != RTC_SUCCESS) return ret;
    if ((ret = init_control_shared_memory()) != RTC_SUCCESS) return ret;
    if ((ret = init_signal_semaphores()) != RTC_SUCCESS) return ret;

    return RTC_SUCCESS;
}


static void *signal_handler_thread(void *data) {
    int signal_number = *(int*)data;
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, signal_number);

    while (true) {
        // log_debug("[%ld] Waiting for signal!", pthread_self());
        err = sigwaitinfo(&sig_set, &sig_info);
        if (err == -1) {
            // log_error("[%ld] sigwaitinfo() call failed!", pthread_self());
        } else {
            // log_debug("[%ld] Processing signal %d from process %d", pthread_self(), sig_info.si_signo, sig_info.si_pid);

            if (sig_info.si_signo == SIGUSR1) {
                int shmsegIdx = getAssignedShmsegIdx(sig_info.si_pid);

                if (shmsegIdx == NO_IDX) {
                    // log_error("Received signal SIGUSR1 from unrecognized process %d, continuing...", sig_info.si_pid);
                    continue;
                } else {
                    // Allow thread allocated for pid to process
                    if (sem_post(&(self.sig_semaphore[shmsegIdx]))) {
                        // log_error("sem_post() call failed, continuing...");
                        continue;
                    }
                }
            } else if (sig_info.si_signo == SIGUSR2) {
                switch (self.control_shmp->request) {
                case STOP_MASTER:
                    handle_configurator_stop_master_request();
                    break;
                case DELETE_SLAVE:
                    handle_configurator_delete_slave_request();
                    break;
                case CONNECT_SLAVE:
                    if (handle_configurator_connect_slave_request() != RTC_SUCCESS) {
                        // ACK response is send to configurator when ACK response received from slave
                        self.control_shmp->response = NACK;
                        if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                            // log_error("Failed to send NACK signal to configurator!");
                        }
                    }
                    break;
                case DISCONNECT_SLAVE:
                    handle_configurator_disconnect_slave_request();
                    break;
                case START_SLAVE_CYCLE:
                    if (handle_start_cycle_slave_request() != RTC_SUCCESS) {
                        // ACK response is send to configurator when ACK response received from slave
                        self.control_shmp->response = NACK;
                        if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                            // log_error("Failed to send NACK signal to configurator!");
                        }
                    }
                    break;
                case STOP_SLAVE_CYCLE:
                    if (handle_stop_cycle_slave_request() != RTC_SUCCESS) {
                        // ACK response is send to configurator when ACK response received from slave
                        self.control_shmp->response = NACK;
                        if (kill(self.control_shmp->configurator_pid, SIGUSR2)) {
                            // log_error("Failed to send NACK signal to configurator!");
                        }
                    }
                    break;
                default:
                    // log_error("Received unknown command from configurator process %d", self.control_shmp->configurator_pid);
                    break;
                }
            }
        }
    }
    return NULL;
}


int send_change_name_slave_request(int shmsegIdx) {
    pid_t slave_pid = self.shmp->slave_shmseg[shmsegIdx].pid;

    self.shmp->slave_shmseg[shmsegIdx].req_m_to_s = CHANGE_SLAVE_NAME;

    log_debug("send_change_name_slave_request to process %d", slave_pid);
    if (kill(slave_pid, SIGUSR1)) {
        log_error("Failed to send change name slave request to process %d!", slave_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_connect_slave_request(shmseg_t *slave_shmseg) {
    slave_shmseg->req_m_to_s = CONNECT_SLAVE;

    log_debug("send_connect_slave_request to process %d", slave_shmseg->pid);
    if (kill(slave_shmseg->pid, SIGUSR1)) {
        log_error("Failed to send connect slave request to process %d!", slave_shmseg->pid);
        // TODO 9: Reset slave shared memory
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_start_cycle_slave_request() {
    pid_t slave_pid = self.control_shmp->affected_slave_pid;

    int shmsegIdx = getAssignedShmsegIdx(slave_pid);
    if (shmsegIdx == NO_IDX) {
        // log_debug("Process %d has no assigned index!", slave_pid);
        return RTC_ERROR;
    }

    if (self.shmp->slave_shmseg[shmsegIdx].communication_cycle_us == NOT_SET_ERROR_COMMUNICATION_CYCLE_MS) {
        // log_error("Process %d has not been set communication cycle variable!", slave_pid);
        return RTC_ERROR;
    }

    self.shmp->slave_shmseg[shmsegIdx].req_m_to_s = START_SLAVE_CYCLE;

    // log_debug("send_start_cycle_slave_request to process %d", slave_pid);
    if (kill(slave_pid, SIGUSR1)) {
        // log_error("Failed to send start cycle slave request to process %d!", slave_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_stop_cycle_slave_request() {
    pid_t slave_pid = self.control_shmp->affected_slave_pid;

    int shmsegIdx = getAssignedShmsegIdx(slave_pid);
    if (shmsegIdx == NO_IDX) {
        // log_debug("Process %d has no assigned index!", slave_pid);
        return RTC_ERROR;
    }

    self.shmp->slave_shmseg[shmsegIdx].req_m_to_s = STOP_SLAVE_CYCLE;

    // log_debug("send_stop_cycle_slave_request to process %d", slave_pid);
    if (kill(slave_pid, SIGUSR1)) {
        // log_error("Failed to send stop cycle slave request to process %d!", slave_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}


void handle_slave_request_errors(shmseg_t *slave_shmseg) {
    log_error("Received from slave process [%d]: %s",
        slave_shmseg->pid,
        slave_shmseg->error_string);
    memset(slave_shmseg->error_string, STRING_VALUE_UNDEFINED, STRING_SIZE);
}

int handle_configurator_stop_master_request() {
    // TODO
    return RTC_SUCCESS;
}

int handle_configurator_delete_slave_request() {
    // TODO
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
    // TODO
    return RTC_SUCCESS;
}

int handle_start_cycle_slave_request() {
    return send_start_cycle_slave_request();
}

int handle_stop_cycle_slave_request() {
    return send_stop_cycle_slave_request();
}


int main(int argc, char *argv[]) {
    init(); // TODO: Failed initialization => end program

    int ret;
    pthread_t tid1;
    pthread_t tid2;
    int signal_number1 = SIGUSR1;
    int signal_number2 = SIGUSR2;

    // Create signal listening threads
    ret = pthread_create(&tid1, NULL, signal_handler_thread, (void*)&signal_number1);
    if (ret) {
        log_error("pthread_create() to create signal handler thread failed");
        return RTC_ERROR;
    }
    ret = pthread_create(&tid2, NULL, signal_handler_thread, (void*)&signal_number2);
    if (ret) {
        log_error("pthread_create() to create signal handler thread failed");
        return RTC_ERROR;
    }

    // Full initialization complete, send response to configurator
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
