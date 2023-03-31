#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/shm.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "configurator.h"



static configurator_context_t self = { .shmp = NULL, .control_shmp = NULL };



static int block_signals() {
    sigset_t block_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &block_mask, NULL) == -1) {
        fprintf(stderr, "Can't block signals!\n");
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

// Returns assigned index if successful. NO_IDX if failed.
static int getAssignedShmsegIdx(pid_t pid) {
    int shmsegIdx = pid % MAX_SLAVES;

    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (self.shmp->slave_shmseg[shmsegIdx].pid == pid) {
            return shmsegIdx;
        }
        ++shmsegIdx;
        shmsegIdx %= MAX_SLAVES;
    }

    fprintf(stderr, "Failed to find assigned shared memory segment index for process %d\n", pid);
    return NO_IDX;
}

static void print_slave_data_by_pid(pid_t pid) {
    int shmsegIdx = getAssignedShmsegIdx(pid);
    if (shmsegIdx == NO_IDX) return;

    printf("pid: %d\n", pid);
    printf("name: %s\n", self.shmp->slave_shmseg[shmsegIdx].name);
    printf("available parameters: ");
    if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & STRING_PARAMETER_BIT) {
        printf("string paramater, ");
    }
    if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & INT_PARAMETER_BIT) {
        printf("int paramater, ");
    }
    if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & BOOL_PARAMETER_BIT) {
        printf("bool paramater\n");
    }
}

static void print_main_menu() {
    printf("\n");
    for (int i = 0; i < CHANGE_SLAVE_NAME; ++i) {
        switch (i) {
        case START_MASTER:
            printf("%d: Start master\n", i);
            break;
        case STOP_MASTER:
            printf("%d: Stop master\n", i);
            break;
        case CREATE_SLAVE:
            printf("%d: Create slave\n", i);
            break;
        case DELETE_SLAVE:
            printf("%d: Delete slave\n", i);
            break;
        case CONNECT_SLAVE:
            printf("%d: Connect slave\n", i);
            break;
        case DISCONNECT_SLAVE:
            printf("%d: Disconnect slave\n", i);
            break;
        case START_SLAVE_CYCLE:
            printf("%d: Start slave cycle\n", i);
            break;
        case STOP_SLAVE_CYCLE:
            printf("%d: Stop slave cycle\n", i);
            break;
        }
    }

}

static void read_req_code() {
    int req_code;
    size_t line_length = MAX_LINE_LENGTH;
    printf("\nInsert command number: ");

    char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

    getline(&line, &line_length, stdin);

    if (sscanf(line, "%d", &req_code) != 1) {
        fprintf(stderr, "Invalid command number. Please insert a number!\n");
        req_code = -1;
    }
    self.control_shmp->request = req_code;

    free(line);
    printf("\n");
}

static int init_control_shared_memory() {
    int shmid;

    // Get or create if not existing shared memory
    shmid = shmget(CONTROL_SHARED_MEMORY_KEY, sizeof(control_shm_t), 0666 | IPC_CREAT);
    if (shmid == -1) {
        fprintf(stderr, "shmget() call failed to create shared memory!\n");
        return RTC_ERROR;
    }
    // Attach to shared memory
    self.control_shmp = shmat(shmid, NULL, 0);
    if (self.control_shmp == (void*) -1) {
        fprintf(stderr, "shmmat() call failed to attach to shared memory!\n");
        return RTC_ERROR;
    }

    memset(self.control_shmp, 0, sizeof(control_shm_t));

    self.control_shmp->configurator_pid = getpid();
    fprintf(stderr, "Set self.control_shmp->configurator_pid to %d\n", self.control_shmp->configurator_pid);

    return RTC_SUCCESS;
}

static int init_shared_memory() {
    int shmid;

    // Get or create if not existing shared memory
    shmid = shmget(SHARED_MEMORY_KEY, sizeof(shm_t), 0666);
    if (shmid == -1) {
        fprintf(stderr, "shmget() call failed to create shared memory!\n");
        return RTC_ERROR;
    }
    // Attach to shared memory
    self.shmp = shmat(shmid, NULL, 0);
    if (self.shmp == (void*) -1) {
        fprintf(stderr, "shmmat() call failed to attach to shared memory!\n");
        return RTC_ERROR;
    }

    return RTC_SUCCESS;
}

static int final() {
    int ret = RTC_SUCCESS;

    // Deattach from control shared memory
    if (shmdt(self.control_shmp)) {
        fprintf(stderr, "shmdt() call failed!\n");
        ret = RTC_ERROR;
    }
    self.control_shmp = NULL;

    // Deattach from shared memory
    if (shmdt(self.shmp)) {
        fprintf(stderr, "shmdt() call failed!\n");
        ret = RTC_ERROR;
    }
    self.shmp = NULL;

    return ret;
}

int send_start_cycle_slave_request() {
    self.control_shmp->request = START_SLAVE_CYCLE;
    if (kill(self.master_pid, SIGUSR2)) {
        fprintf(stderr, "Failed to send signal SIGUSR2 to master process %d\n", self.master_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_connect_slave_request() {
    if (kill(self.master_pid, SIGUSR2)) {
        fprintf(stderr, "Failed to send signal SIGUSR2 to master process %d\n", self.master_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

void wait_start_slave_cycle_response() {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    struct timespec timeout = { .tv_sec = WAIT_TIMEOUT_SECONDS };

    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR2);

    err = sigtimedwait(&sig_set, &sig_info, &timeout);
    if (err == -1) {
        perror("sigtimedwait() call failed in wait_start_slave_cycle_response()!");
    } else {
        switch (self.control_shmp->response)
        {
        case NACK:
            printf("Start slave cycle slave request failed! (Maybe already started?)\n");
            break;
        case ACK:
            printf("Start slave cycle slave request succeded!\n");
            break;
        default:
            fprintf(stderr, "Unrecognized response received!");
            break;
        }
        self.control_shmp->request = NO_REQUEST;
        self.control_shmp->response = NO_RESPONSE;
    }
}

void wait_connect_slave_response(pid_t pid) {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    struct timespec timeout = { .tv_sec = WAIT_TIMEOUT_SECONDS };

    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR2);

    err = sigtimedwait(&sig_set, &sig_info, &timeout);
    if (err == -1) {
        perror("sigtimedwait() call failed in wait_connect_slave_response()!");
    } else {
        switch (self.control_shmp->response)
        {
        case NACK:
            printf("Connect slave request failed!\n");
            break;
        case ACK:
            printf("Connect slave request succeded!\n");
            print_slave_data_by_pid(pid);
            break;
        default:
            fprintf(stderr, "Unrecognized response received!");
            break;
        }
        self.control_shmp->request = NO_REQUEST;
        self.control_shmp->response = NO_RESPONSE;
    }
}

int wait_master_started_signal() {
    int err;
    int ret;
    sigset_t sig_set;
    siginfo_t sig_info;
    struct timespec timeout = { .tv_sec = WAIT_TIMEOUT_SECONDS };

    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR2);

    err = sigtimedwait(&sig_set, &sig_info, &timeout);
    if (err == -1) {
        perror("sigtimedwait() call failed in wait_master_started_signal()!");
        return RTC_ERROR;
    } else {
        if ((ret = init_shared_memory()) != RTC_SUCCESS) return ret;
    }
    return RTC_SUCCESS;
}

int main(int argc, char *argv[]) {
    int ret;

    if ((ret = block_signals()) != RTC_SUCCESS) return ret;
    if ((ret = init_control_shared_memory()) != RTC_SUCCESS) return ret;

    while(true) {
        print_main_menu();
        read_req_code();
        switch(self.control_shmp->request) {
        case START_MASTER:
        {
            int fork_pid = fork();
            if (fork_pid == 0) {
                // Child process
                char *arg_Ptr[2];
                arg_Ptr[0] = "master";
                arg_Ptr[1] = NULL;

                ret = execv(MASTER_BIN_PATH, arg_Ptr);
                if (ret == -1) {
                    fprintf(stderr, "execv() call failed!\n");
                }
            } else {
                // Parent process
                self.master_pid = fork_pid;
                if ((ret = wait_master_started_signal()) != RTC_SUCCESS) return ret;
                printf("Started master process %d\n\n", fork_pid);
            }
            break;
        }
        case STOP_MASTER:
            printf("TODO: Stop master\n");
            break;
        case CREATE_SLAVE:
        {
            char slave_name[SLAVE_NAME_SIZE];
            memset(slave_name, 0, SLAVE_NAME_SIZE * sizeof(char));

            printf("Set slave name: ");
            {
                size_t read_characters;
                size_t line_length = MAX_LINE_LENGTH;
                char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                read_characters = getline(&line, &line_length, stdin);

                if (read_characters < 2) {
                    printf("Minimum name length is 1 character!\n");
                    free(line);
                    break;
                }
                if (line[0] == ' ') {
                    printf("Name can't start with space character!");
                    free(line);
                    break;
                }

                sscanf(line, "%127s", slave_name);
                free(line);
            }
            printf("\n");

            printf("Set slave parameters (<XYZ> where X = STRING, Y = INT, Z = BOOL; X | Y | Z = 1 => parameter available otherise not available): ");
            char XYZ[] = "000";
            {
                size_t read_characters;
                size_t line_length = MAX_LINE_LENGTH;
                char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                read_characters = getline(&line, &line_length, stdin);
                if (read_characters == -1) {
                    printf("Failed to read parameters line\n");
                }

                if (line[0] == '1')
                    XYZ[0] = '1';
                if (line[1] == '1')
                    XYZ[1] = '1';
                if (line[2] == '1')
                    XYZ[2] = '1';

                free(line);
            }
            printf("\n");
            
            int fork_pid = fork();
            if (fork_pid == 0) {
                // Child process
                char *arg_Ptr[3];
                arg_Ptr[0] = "slave";
                arg_Ptr[1] = slave_name;
                arg_Ptr[2] = XYZ;
                arg_Ptr[3] = NULL;

                ret = execv(SLAVE_BIN_PATH, arg_Ptr);
                if (ret == -1) {
                    fprintf(stderr, "execv() call failed!\n");
                }
            } else {
                // Parent process
                // fork.pid is slave_pid
                printf("Started slave process %d with name %s and available parameters %s\n\n", fork_pid, slave_name, XYZ);
            }
            break;
        }
        case DELETE_SLAVE:
            printf("TODO: Delete slave\n");
            break;
        case CONNECT_SLAVE:
        {
            pid_t slave_pid;

            printf("Insert slave pid to connect: ");

            {
                size_t read_characters;
                size_t line_length = MAX_LINE_LENGTH;
                char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                read_characters = getline(&line, &line_length, stdin);

                if (read_characters < 1 || line[0] < '0' || line[0] > '9') {
                    fprintf(stderr, "Invalid process id!\n");
                    free(line);
                    break;
                }

                sscanf(line, "%d", &slave_pid);
                free(line);
            }
            printf("\n");

            // TODO 5: save slave_pids in self structure to be able to see pids of disconnected slaves (can see connected slaves through self.shmp)

            self.control_shmp->affected_slave_pid = slave_pid;
            
            if (send_connect_slave_request()) {
                // Failed to send connect request
                break;
            }

            wait_connect_slave_response(slave_pid);

            break;
        }
        case DISCONNECT_SLAVE:
            printf("TODO: Disconnect slave\n");
            break;
        case START_SLAVE_CYCLE:
        {
            int connected_pids_iter = 0;
            pid_t connected_pids[MAX_SLAVES];
            pid_t slave_pid;
            bool valid_pid = false;

            // Inform user of available connected slave pids
            printf("Available connected slaves pids: ");
            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.shmp->slave_shmseg[i].is_connected) {
                    printf(" %d,", self.shmp->slave_shmseg[i].pid);
                    connected_pids[connected_pids_iter] = self.shmp->slave_shmseg[i].pid;
                    ++connected_pids_iter;
                }
            }
            printf("\n");
            printf("Insert slave pid to start cycle: ");
            {
                size_t read_characters;
                size_t line_length = MAX_LINE_LENGTH;
                char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                read_characters = getline(&line, &line_length, stdin);

                if (read_characters < 1 || line[0] < '0' || line[0] > '9') {
                    fprintf(stderr, "Invalid process id!\n");
                    free(line);
                    break;
                }

                sscanf(line, "%d", &slave_pid);
                free(line);
            }
            printf("\n");

            for (int i = 0; i < connected_pids_iter; ++i) {
                if (slave_pid == connected_pids[i]) {
                    valid_pid = true;
                    break;
                }
            }
            if (!valid_pid) {
                printf("Not a valid slave pid!\n");
                break;
            }

            int shmsegIdx = getAssignedShmsegIdx(slave_pid);

            // Ask user which available parameters to request
            if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & STRING_PARAMETER_BIT) {
                char parameter_response;
                printf("request string parameter ? (y/n) ");
                {
                    size_t read_characters;
                    size_t line_length = MAX_LINE_LENGTH;
                    char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                    read_characters = getline(&line, &line_length, stdin);

                    if (read_characters < 1) {
                        parameter_response = 'n';
                        free(line);
                        break;
                    }

                    sscanf(line, "%c", &parameter_response);
                    free(line);
                }
                switch (parameter_response)
                {
                case 'y':
                case 'Y':
                    self.shmp->slave_shmseg[shmsegIdx].requested_parameters |= STRING_PARAMETER_BIT;
                    break;
                }
            }
            if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & INT_PARAMETER_BIT) {
                char parameter_response;
                printf("request int parameter ? (y/n) ");
                {
                    size_t read_characters;
                    size_t line_length = MAX_LINE_LENGTH;
                    char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                    read_characters = getline(&line, &line_length, stdin);

                    if (read_characters < 1) {
                        parameter_response = 'n';
                        free(line);
                        break;
                    }

                    sscanf(line, "%c", &parameter_response);
                    free(line);
                }
                switch (parameter_response)
                {
                case 'y':
                case 'Y':
                    self.shmp->slave_shmseg[shmsegIdx].requested_parameters |= INT_PARAMETER_BIT;
                    break;
                }
            }
            if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & BOOL_PARAMETER_BIT) {
                char parameter_response;
                printf("request bool parameter ? (y/n) ");
                {
                    size_t read_characters;
                    size_t line_length = MAX_LINE_LENGTH;
                    char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                    read_characters = getline(&line, &line_length, stdin);

                    if (read_characters < 1) {
                        parameter_response = 'n';
                        free(line);
                        break;
                    }

                    sscanf(line, "%c", &parameter_response);
                    free(line);
                }
                switch (parameter_response)
                {
                case 'y':
                case 'Y':
                    self.shmp->slave_shmseg[shmsegIdx].requested_parameters |= BOOL_PARAMETER_BIT;
                    break;
                }
            }
            printf("\n");

            printf("Insert requested parameters time interval (ms): ");
            {
                long int req_cycle_interval_ms;
                size_t line_length = MAX_LINE_LENGTH;

                char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                getline(&line, &line_length, stdin);

                if (sscanf(line, "%ld", &req_cycle_interval_ms) != 1) {
                    fprintf(stderr, "Invalid number. Please insert a number!\n");
                    break;
                }
                if (req_cycle_interval_ms <= 0) {
                    fprintf(stderr, "Invalid number. Can't be negative number or 0!\n");
                    break;
                }
                self.shmp->slave_shmseg[shmsegIdx].communication_cycle_us = req_cycle_interval_ms * 1000;

                free(line);
            }
            printf("\n");

            self.control_shmp->affected_slave_pid = slave_pid;

            if (send_start_cycle_slave_request() == RTC_SUCCESS) {
                wait_start_slave_cycle_response();
            }
            break;
        }
        case STOP_SLAVE_CYCLE:
            printf("TODO: Stop slave cycle\n");
            // TODO 2: Create stop slave cycle request and send it
            break;
        default:
            printf("Unrecognized request command, continuing...\n");
            break;
        }
    }

    // TODO: Figure out how to handle CTRL+C killing child processes
    // because if configurator exits normally (return statement)
    // children processes become orphans and get adopted by init process.
    // Currently not possible to exit normally. (easiest/fastest way)
    
    return final();
}
