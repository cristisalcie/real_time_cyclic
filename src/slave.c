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



static slave_context_t self;



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
            printf("slave[%d]: Got shared mem idx %d\n", pid, shmsegIdx);
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

    long int communication_cycle_us = elapsed_microseconds - self.shmseg->communication_cycle_us;

    self.excess_communication_cycle_interval_ms = communication_cycle_us / 1000.0;

    self.last_communication_timeval.tv_sec = current_timeval.tv_sec;
    self.last_communication_timeval.tv_usec = current_timeval.tv_usec;

    if (self.excess_communication_cycle_interval_ms > self.shmseg->communication_cycle_us / 2000.0) {
        // Exceeded half of communication cycle
        set_error("Exceeded communication cycle set time by %lf miliseconds", self.excess_communication_cycle_interval_ms);
    }
}


static void *slave_communication_cycle_detached_thread(void *ignore) {
    long int sleep_us = ERROR_NOT_SET_COMMUNICATION_CYCLE_MS;

    while (true) {
        fprintf(stderr, "slave[%d]: before waiting allow_communication_cycle\n", getpid());

        if (sem_wait(&self.allow_communication_cycle)) {
            fprintf(stderr, "slave[%d]: sem_wait() call failed!\n", getpid());
            return NULL;
        }

        fprintf(stderr, "slave[%d]: after waiting allow_communication_cycle\n", getpid());

        if (self.shmseg->communication_cycle_us == ERROR_NOT_SET_COMMUNICATION_CYCLE_MS) {
            // NOTE: (already done in master)
        }

        if (sleep_us == ERROR_NOT_SET_COMMUNICATION_CYCLE_MS) {
            sleep_us = self.shmseg->communication_cycle_us;
        }

        if (sleep_us > 0) {
            if (usleep(sleep_us)) {
                // Set error and continue normally
                set_error("usleep() failed!");
                continue;
            }
        }

        int rand_value = rand();
        if (self.shmseg->requested_parameters & STRING_PARAMETER_BIT) {
            char chr_rand = 'A' + rand_value % ('Z' - 'A' + 1) ;
            self.shmseg->string_value[0] = chr_rand;
            self.shmseg->string_value[1] = chr_rand + 1;
            self.shmseg->string_value[2] = chr_rand + 2;
        }
        if (self.shmseg->requested_parameters & INT_PARAMETER_BIT) {
            self.shmseg->int_value = rand_value;
        }
        if (self.shmseg->requested_parameters & BOOL_PARAMETER_BIT) {
            self.shmseg->bool_value = rand_value % 2;
        }

        calculate_parameter_communication_interval();


        if (sem_post(&self.allow_communication_cycle)) {
            set_error("sem_post() call failed on allow_communication_cycle! Stopping communication cycle");
            self.shmseg->cycle_started = false;
        }
        /* handle_stop_communication_cycle can now sem_wait on allow_communication_cycle allowing
         * signal_handler_thread to unlock itself and process master response
         */

        sleep_us = self.shmseg->communication_cycle_us - send_master_signal_master_parameter_request();
    }

    return NULL;
}


static int create_communication_cycle_detached_thread() {
    pthread_t tid;
    pthread_attr_t attr;
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret) {
        fprintf(stderr, "slave[%d]: pthread_attr_init() call failed!\n", getpid());
        return RTC_ERROR;
    }

    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (ret) {
        fprintf(stderr, "slave[%d]: pthread_attr_setdetachstate() call failed!\n", getpid());
        return RTC_ERROR;
    }

    ret = pthread_create(&tid, &attr, slave_communication_cycle_detached_thread, NULL);
    if (ret) {
        fprintf(stderr, "slave[%d]: pthread_create() call failed!\n", getpid());
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

static int first_signal_ever_setup() {
    if (self.shmseg == NULL) {
        // First time getting shared memory index.
        int shmsegIdx = getAssignedShmsegIdx(getpid());
        if (shmsegIdx == NO_IDX) {
            // Process has not been assigned a shared memory segment yet.
            fprintf(stderr, "Process %d has no assigned shared memory index, continuing...\n", getpid());
            // Ignore since there is no variable to write error in
            return RTC_ERROR;
        }
        self.shmseg = &self.shmp->slave_shmseg[shmsegIdx];
    }
    return RTC_SUCCESS;
}



static void *signal_handler_thread(void *ignore) {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR1);

    while (true) {
        err = sigwaitinfo(&sig_set, &sig_info);
        if (err == -1) {
            fprintf(stderr, "slave[%d]: sigwaitinfo() call failed!\n", getpid());
        } else {
            if (sig_info.si_pid != self.shmp->master_pid) {
                fprintf(stderr, "Received signal from process %d."
                    " Only accepting signals from master process %d, continuing...\n",
                    sig_info.si_pid, self.shmp->master_pid);
                // Ignore and move on
                continue;
            }
            
            if (sig_info.si_signo == SIGUSR1) {
                if (first_signal_ever_setup() != RTC_SUCCESS) {
                    continue;
                }

                // TODO: sem_m_to_s_request_val currently could suffer from starvation.
                // Possible solution:
                // Add a variable to change order of semaphore checking.

                int sem_s_to_m_request_val = 1;
                if (sem_getvalue(&self.shmseg->sem_s_to_m_request, &sem_s_to_m_request_val)) {
                    fprintf(stderr, "slave[%d]: sem_getvalue() call failed!\n", getpid());
                    continue;
                }

                if (sem_s_to_m_request_val <= 0) {
                    // Receive response signal, increment semaphore, continue to next signal
                    handle_master_response();
                } else {
                    int sem_m_to_s_request_val = 1;
                    if (sem_getvalue(&self.shmseg->sem_m_to_s_request, &sem_m_to_s_request_val)) {
                        fprintf(stderr, "slave[%d]: sem_getvalue() call failed!\n", getpid());
                        continue;
                    }

                    if (sem_m_to_s_request_val <= 0) {
                        // Handle request and send response
                        handle_master_request();
                    } else {
                        // No response towards master unhandled (warn/error)
                        fprintf(stderr, "slave[%d]: No request towards slave unhandled!\n", getpid());
                    }
                }
            }
        }
    }
    return NULL;
}


int destroy_allow_communication_cycle_semaphore() {
    if (sem_destroy(&self.allow_communication_cycle)) {
        fprintf(stderr, "slave[%d]: sem_destroy() call failed!\n", getpid());
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

int init_allow_communication_cycle_semaphore() {
    if (sem_init(&self.allow_communication_cycle, 0, 0)) {
        fprintf(stderr, "slave[%d]: sem_init() call failed!\n", getpid());
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

int init_shared_memory() {
    int shmid;

    // Get or create if not existing shared memory
    shmid = shmget(SHARED_MEMORY_KEY, sizeof(shm_t), 0666);
    if (shmid == -1) {
        fprintf(stderr, "slave[%d]: shmget() call failed to get shared memory id!\n", getpid());
        perror("shmget() failed!");
        return RTC_ERROR;
    }

    // Attach to shared memory
    self.shmp = shmat(shmid, NULL, 0);
    if (self.shmp == (void*) -1) {
        fprintf(stderr, "slave[%d]: shmmat() call failed to attach to shared memory!\n", getpid());
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

int init(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "slave[%d]: Syntax: %s <slave_name> <available_parameters>\n", getpid(), argv[0]);
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

int final() {
    int ret = RTC_SUCCESS;

    ret = destroy_allow_communication_cycle_semaphore();

    // Deattach from shared memory
    if (shmdt(self.shmp)) {
        fprintf(stderr, "slave[%d]: shmdt() call failed!\n", getpid());
        ret = RTC_ERROR;
    }
    self.shmp = NULL;

    return ret;
}


void send_master_ack_response() {
    fprintf(stderr, "slave[%d]: send_master_ack_response()\n", getpid());
    self.shmseg->res_s_to_m = ACK;
    if (kill(self.shmp->master_pid, SIGUSR1)) {
        fprintf(stderr, "slave[%d]: Failed to send response back to master!\n", getpid());
    }
}

void send_master_nack_response() {
    fprintf(stderr, "slave[%d]: send_master_nack_response()\n", getpid());
    self.shmseg->res_s_to_m = NACK;
    if (kill(self.shmp->master_pid, SIGUSR1)) {
        fprintf(stderr, "slave[%d]: Failed to send response back to master!\n", getpid());
    }
}

long int send_master_signal_master_parameter_request() {
    long int elapsed_communication_cycle_us = 0;
    long int time_segment_us = self.shmseg->communication_cycle_us
                                / WAIT_MASTER_RESPONSE_TIMEOUT_SEGMENTS;
    int threshold = 0;

    // First time getting in here, for sure semaphore is 1
    while (sem_trywait(&self.shmseg->sem_s_to_m_request)) {
        if (threshold >= WAIT_MASTER_RESPONSE_TIMEOUT_SEGMENTS) {
            set_error("Master failed to respond to request!");
            break;
        }

        usleep(time_segment_us);

        elapsed_communication_cycle_us += time_segment_us;
        ++threshold;
        // Let signal be lost, continue as nothing happened.
    }

    fprintf(stderr, "slave[%d]: threshold >= %d\n", getpid(), threshold);
    fprintf(stderr, "slave[%d]: Send data request to master\n", getpid());


    int allow_communication_cycle_val = 1;
    if (sem_getvalue(&self.allow_communication_cycle, &allow_communication_cycle_val)) {
        fprintf(stderr, "slave[%d]: sem_getvalue() call failed!\n", getpid());
        time_segment_us = 0;
        return time_segment_us;
    }
    if (allow_communication_cycle_val <= 0) {
        /* Communication cycle stopped => set time_segment_us to 0 and sem_s_to_m_request to 1
         * because next time in here will be completely new.
         */
        time_segment_us = 0;
        if (sem_post(&self.shmseg->sem_s_to_m_request)) {
            fprintf(stderr, "slave[%d]: sem_post() call failed!\n", getpid());
        }
        return time_segment_us;
    }


    self.shmseg->req_s_to_m = SIGNAL_MASTER_PARAMETER;
    if (kill(self.shmp->master_pid, SIGUSR1)) {
        fprintf(stderr, "slave[%d]: Failed to send \"signal master parameter\" signal to master!\n", getpid());
    }

    return time_segment_us;
}

void handle_signal_master_parameter_ack_response() {
    fprintf(stderr, "slave[%d]: received ACK for processing new parameter cycle.\n", getpid());
}

void handle_signal_master_parameter_nack_response() {
    fprintf(stderr, "slave[%d]: received NACK for processing new parameter cycle.\n", getpid());
}

void handle_signal_master_parameter_unrecognized_response() {
    fprintf(stderr, "slave[%d]: Unrecognized response received from master\n", getpid());
}


void handle_connect_slave_request() {
    // Fill shared memory structure
    strcpy(self.shmseg->name, self.name);
    self.shmseg->available_parameters = self.available_parameters;

    // Send master response
    send_master_ack_response();
}

void handle_disconnect_slave_request() {
    self.last_communication_timeval.tv_sec = 0;
    self.last_communication_timeval.tv_usec = 0;
    self.excess_communication_cycle_interval_ms = 0;
    send_master_ack_response();
    self.shmseg = NULL;
}

void handle_change_name_slave_request() {
    unsigned int prev_change_name_idx = self.change_name_idx;

    if (self.change_name_idx) {
        // Erase previously added number from string name
        for (int i = SLAVE_NAME_SIZE - 1; i > 0; --i) {
            if (self.name[i] == '_') {
                self.name[i] = '\0';
                self.shmseg->name[i] = '\0';
                break;
            }
            self.name[i] = '\0';
            self.shmseg->name[i] = '\0';
        }
    }
    ++prev_change_name_idx;
    if (prev_change_name_idx < self.change_name_idx) {
        // Index overflow send NACK
        set_error("Change name index overflow!");
        send_master_nack_response();
        return;
    }
    ++self.change_name_idx;

    sprintf(self.name, "%s_%u", self.shmseg->name, self.change_name_idx);
    strcpy(self.shmseg->name, self.name);

    send_master_ack_response();
}

void handle_start_cycle_slave_request() {
    if (self.shmseg->cycle_started) {
        set_error("Slave cycle already started!");
        send_master_nack_response();
        return;
    }

    self.shmseg->cycle_started = true;

    if (sem_post(&self.allow_communication_cycle)) {
        set_error("sem_post() failed on allow_communication_cycle!");
        send_master_nack_response();
        return;
    }

    send_master_ack_response();
}

void handle_stop_cycle_slave_request() {
    if (!self.shmseg->cycle_started) {
        set_error("Slave cycle already stopped!");
        send_master_nack_response();
        return;
    }

    self.shmseg->cycle_started = false;

    fprintf(stderr, "slave[%d]: handle_stop_cycle_slave_request before getting semaphore allow_communication_cycle\n", getpid());
    sem_wait(&self.allow_communication_cycle);
    fprintf(stderr, "slave[%d]: handle_stop_cycle_slave_request after getting semaphore allow_communication_cycle\n", getpid());

    send_master_ack_response();
}

void handle_unrecognized_request() {
    set_error("Unrecognized request received!");
    send_master_nack_response();
}

void handle_master_request() {
    switch (self.shmseg->req_m_to_s) {
    case CONNECT_SLAVE:
        fprintf(stderr, "slave[%d]: handling CONNECT_SLAVE master request\n", getpid());
        handle_connect_slave_request();
        break;
    case DISCONNECT_SLAVE:
        fprintf(stderr, "slave[%d]: handling DISCONNECT_SLAVE master request\n", getpid());
        handle_disconnect_slave_request();
        break;
    case CHANGE_SLAVE_NAME:
        fprintf(stderr, "slave[%d]: handling CHANGE_SLAVE_NAME master request\n", getpid());
        handle_change_name_slave_request();
        break;
    case START_SLAVE_CYCLE:
        fprintf(stderr, "slave[%d]: handling START_SLAVE_CYCLE master request\n", getpid());
        handle_start_cycle_slave_request();
        break;
    case STOP_SLAVE_CYCLE:
        fprintf(stderr, "slave[%d]: handling STOP_SLAVE_CYCLE master request\n", getpid());
        handle_stop_cycle_slave_request();
        break;
    default:
        fprintf(stderr, "slave[%d]: handling default master request\n", getpid());
        handle_unrecognized_request();
        break;
    }
}


void handle_master_response() {
    switch (self.shmseg->req_s_to_m) {
    case SIGNAL_MASTER_PARAMETER:
        switch (self.shmseg->res_m_to_s) {
        case ACK:
            handle_signal_master_parameter_ack_response();
            break;
        case NACK:
            handle_signal_master_parameter_nack_response();
            break;
        default:
            handle_signal_master_parameter_unrecognized_response();
            break;
        }
        break;
    default:
        fprintf(stderr, "slave[%d]: Unrecognized request received while handling master response\n", getpid());
        break;
    }
    if (sem_post(&self.shmseg->sem_s_to_m_request)) {
        fprintf(stderr, "slave[%d]: sem_post() call failed!\n", getpid());
    }
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

    return final();
}
