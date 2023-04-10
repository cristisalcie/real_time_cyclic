#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/shm.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>

#include "configurator.h"



static configurator_context_t self;



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
static int assignLocalIdx(pid_t pid) {
    int localIdx = pid % MAX_SLAVES;

    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (self.created_slave[localIdx].pid == NO_PID) {
            return localIdx;
        }
        ++localIdx;
        localIdx %= MAX_SLAVES;
    }

    return NO_IDX;
}

// Returns assigned index if successful. NO_IDX if failed.
static int getAssignedLocalIdx(pid_t pid) {
    int localIdx = pid % MAX_SLAVES;

    for (int i = 0; i < MAX_SLAVES; ++i) {
        if (self.created_slave[localIdx].pid == pid) {
            return localIdx;
        }
        ++localIdx;
        localIdx %= MAX_SLAVES;
    }

    return NO_IDX;
}

static void registerSlaveLocally(int slave_pid) {
    ++self.created_slaves_number;
    int shmsegIdx = assignLocalIdx(slave_pid);
    if (shmsegIdx == NO_IDX) {
        printf("Failed to assign local index to process %d!\n", slave_pid);
    }
    self.created_slave[shmsegIdx].pid = slave_pid;
    self.created_slave[shmsegIdx].connected = false;
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

    printf("pid: %d | ", pid);
    printf("name: %s | ", self.shmp->slave_shmseg[shmsegIdx].name);
    printf("connected: %s | ", self.shmp->slave_shmseg[shmsegIdx].is_connected ? "true" : "false");
    printf("cycle started: %s | ", self.shmp->slave_shmseg[shmsegIdx].cycle_started ? "true" : "false");
    printf("available parameters:");
    if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & STRING_PARAMETER_BIT) {
        printf(" string");
    }
    if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & INT_PARAMETER_BIT) {
        printf(" int");
    }
    if (self.shmp->slave_shmseg[shmsegIdx].available_parameters & BOOL_PARAMETER_BIT) {
        printf(" bool");
    }
    printf("\n");
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
        case PRINT_SLAVE_DATA:
            printf("%d: Print slave data\n", i);
            break;
        case AUTOMATIC_TEST:
            printf("%d: Automatic test\n", i);
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

int send_stop_master_request() {
    // TODO: Improve to use SIGUSR2 and wait for response.
    if (kill(self.master_pid, SIGINT)) {
        fprintf(stderr, "Failed to send signal SIGINT to master process %d\n", self.master_pid);
        return RTC_ERROR;
    }
    self.master_pid = NO_PID;
    return RTC_SUCCESS;
}

int send_start_cycle_slave_request(pid_t slave_pid) {
    self.control_shmp->affected_slave_pid = slave_pid;
    if (kill(self.master_pid, SIGUSR2)) {
        fprintf(stderr, "Failed to send signal SIGUSR2 to master process %d\n", self.master_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_stop_cycle_slave_request(pid_t slave_pid) {
    self.control_shmp->affected_slave_pid = slave_pid;
    if (kill(self.master_pid, SIGUSR2)) {
        fprintf(stderr, "Failed to send signal SIGUSR2 to master process %d\n", self.master_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_delete_slave_request(pid_t slave_pid) {
    // TODO: Improve to use SIGUSR1 and wait for response.
    if (kill(slave_pid, SIGINT)) {
        fprintf(stderr, "Failed to send signal SIGINT to slave process %d\n", slave_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_connect_slave_request(pid_t slave_pid) {
    self.control_shmp->affected_slave_pid = slave_pid;
    if (kill(self.master_pid, SIGUSR2)) {
        fprintf(stderr, "Failed to send signal SIGUSR2 to master process %d\n", self.master_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

int send_disconnect_slave_request(pid_t slave_pid) {
    self.control_shmp->affected_slave_pid = slave_pid;
    printf("request is %d\n", self.control_shmp->request);
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
    }
}

void wait_stop_slave_cycle_response() {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    struct timespec timeout = { .tv_sec = WAIT_TIMEOUT_SECONDS };

    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR2);

    err = sigtimedwait(&sig_set, &sig_info, &timeout);
    if (err == -1) {
        perror("sigtimedwait() call failed in wait_stop_slave_cycle_response()!");
    } else {
        switch (self.control_shmp->response)
        {
        case NACK:
            printf("Stop slave cycle slave request failed! (Maybe already stopped?)\n");
            break;
        case ACK:
            printf("Stop slave cycle slave request succeded!\n");
            break;
        default:
            fprintf(stderr, "Unrecognized response received!");
            break;
        }
    }
}

void wait_connect_slave_response(pid_t slave_pid) {
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
            int localIdx = getAssignedLocalIdx(slave_pid);
            self.created_slave[localIdx].connected = true;
            break;
        default:
            fprintf(stderr, "Unrecognized response received!");
            break;
        }
    }
}

void wait_disconnect_slave_response(pid_t slave_pid) {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;
    struct timespec timeout = { .tv_sec = WAIT_TIMEOUT_SECONDS };

    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR2);

    err = sigtimedwait(&sig_set, &sig_info, &timeout);
    if (err == -1) {
        perror("sigtimedwait() call failed in wait_disconnect_slave_response()!");
    } else {
        switch (self.control_shmp->response)
        {
        case NACK:
            printf("Disconnect slave request failed!\n");
            break;
        case ACK:
            printf("Disconnect slave request succeded!\n");
            int localIdx = getAssignedLocalIdx(slave_pid);
            self.created_slave[localIdx].connected = false;
            break;
        default:
            fprintf(stderr, "Unrecognized response received!");
            break;
        }
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

    memset(&self, 0, sizeof(configurator_context_t));

    memset(self.created_slave, 0, MAX_SLAVES * sizeof(created_slave_t));
    for (int i = 0; i < MAX_SLAVES; ++i) {
        self.created_slave[i].pid = NO_PID;
    }

    self.master_pid = NO_PID;

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
            // TODO: Improve this
            if (self.master_pid == NO_PID) {
                printf("Master not started!\n");
                break;
            }

            // Kill all slaves using SIGINT signal
            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.shmp->slave_shmseg[i].pid == NO_PID) continue;

                send_delete_slave_request(self.shmp->slave_shmseg[i].pid);

                int localIdx = getAssignedLocalIdx(self.shmp->slave_shmseg[i].pid);
                self.created_slave[localIdx].pid = NO_PID;
                self.created_slave[localIdx].connected = false;
            }

            // Kill master using SIGINT signal
            if (send_stop_master_request()) {
                // Failed to send stop master request
                break;
            }
            break;
        case CREATE_SLAVE:
        {
            if (self.created_slaves_number >= MAX_SLAVES) {
                printf("Created maximum number of slaves (%d)!\n", MAX_SLAVES);
                break;
            }

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
                // Parent process, fork_pid is slave_pid
                printf("Started slave process %d with name %s and available parameters %s\n\n", fork_pid, slave_name, XYZ);
                registerSlaveLocally(fork_pid);
            }
            break;
        }
        case DELETE_SLAVE:
        {
            int connected_pids_iter = 0;
            pid_t connected_pids[MAX_SLAVES];
            pid_t slave_pid;
            bool valid_pid = false;

            printf("Disconnected slave pids: ");
            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.created_slave[i].pid == NO_PID) continue;

                if (!self.created_slave[i].connected) {
                    printf("%d ", self.created_slave[i].pid);
                    connected_pids[connected_pids_iter] = self.created_slave[i].pid;
                    ++connected_pids_iter;
                }
            }
            printf("\n");
            printf("Insert slave pid to delete: ");

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


            if (send_delete_slave_request(slave_pid)) {
                // Failed to send delete request
                break;
            }
            int localIdx = getAssignedLocalIdx(slave_pid);
            self.created_slave[localIdx].pid = NO_PID;
            self.created_slave[localIdx].connected = false;
        }
            break;
        case CONNECT_SLAVE:
        {
            int connected_pids_iter = 0;
            pid_t connected_pids[MAX_SLAVES];
            pid_t slave_pid;
            bool valid_pid = false;

            printf("Disconnected slave pids: ");
            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.created_slave[i].pid == NO_PID) continue;

                if (!self.created_slave[i].connected) {
                    printf("%d ", self.created_slave[i].pid);
                    connected_pids[connected_pids_iter] = self.created_slave[i].pid;
                    ++connected_pids_iter;
                }
            }
            printf("\n");
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


            if (send_connect_slave_request(slave_pid)) {
                // Failed to send connect request
                break;
            }

            wait_connect_slave_response(slave_pid);
            break;
        }
        case DISCONNECT_SLAVE:
        {
            int connected_pids_iter = 0;
            pid_t connected_pids[MAX_SLAVES];
            pid_t slave_pid;
            bool valid_pid = false;

            // Inform user of available connected slave pids
            printf("Available connected and not started slave pids: ");
            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.shmp->slave_shmseg[i].is_connected && !self.shmp->slave_shmseg[i].cycle_started) {
                    printf("%d ", self.shmp->slave_shmseg[i].pid);
                    connected_pids[connected_pids_iter] = self.shmp->slave_shmseg[i].pid;
                    ++connected_pids_iter;
                }
            }
            printf("\n");
            printf("Insert slave pid to disconnect: ");
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

            send_disconnect_slave_request(slave_pid);
            wait_disconnect_slave_response(slave_pid);
        }
            break;
        case START_SLAVE_CYCLE:
        {
            int connected_pids_iter = 0;
            pid_t connected_pids[MAX_SLAVES];
            pid_t slave_pid;
            bool valid_pid = false;

            // Inform user of available connected slave pids
            printf("Available connected and not started slave pids: ");
            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.shmp->slave_shmseg[i].is_connected && !self.shmp->slave_shmseg[i].cycle_started) {
                    printf("%d ", self.shmp->slave_shmseg[i].pid);
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


            if (send_start_cycle_slave_request(slave_pid) == RTC_SUCCESS) {
                wait_start_slave_cycle_response();
            }
            break;
        }
        case STOP_SLAVE_CYCLE:
        {
            int started_cycle_pids_iter = 0;
            pid_t started_cycle_pids[MAX_SLAVES];
            pid_t slave_pid;
            bool valid_pid = false;

            printf("Slave pids that started cycle: ");

            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.shmp->slave_shmseg[i].pid == NO_PID) continue;
                if (!self.shmp->slave_shmseg[i].is_connected) continue;
                if (!self.shmp->slave_shmseg[i].cycle_started) continue;

                printf("%d ", self.shmp->slave_shmseg[i].pid);
                started_cycle_pids[started_cycle_pids_iter] = self.shmp->slave_shmseg[i].pid;
                ++started_cycle_pids_iter;
            }
            printf("\n");

            printf("Insert slave pid to stop cycle: ");
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

            for (int i = 0; i < started_cycle_pids_iter; ++i) {
                if (slave_pid == started_cycle_pids[i]) {
                    valid_pid = true;
                    break;
                }
            }
            if (!valid_pid) {
                printf("Not a valid slave pid!\n");
                break;
            }

            if (send_stop_cycle_slave_request(slave_pid) == RTC_SUCCESS) {
                wait_stop_slave_cycle_response();
            }

            break;
        }
        case PRINT_SLAVE_DATA:
            if (!self.shmp) break;

            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.shmp->slave_shmseg[i].pid == NO_PID) {
                    if (self.created_slave[i].pid == NO_PID) continue;

                    printf("Just created slave pid (never connected to master) ");
                    printf("pid: %d\n", self.created_slave[i].pid);

                    continue;
                }

                print_slave_data_by_pid(self.shmp->slave_shmseg[i].pid);
            }            
            break;
        case AUTOMATIC_TEST:
        {
            int num_slaves;
            char slave_name[] = "asd";
            char available_parameters[] = "111";
            char requested_parameters[] = "010";
            // int communication_cycle_us = 1750 * 1000;
            int communication_cycle_us = 500 * 1000;

            printf("Insert number of extra slaves to test application with: ");
            {
                size_t line_length = MAX_LINE_LENGTH;
                char *line = (char*)calloc(MAX_LINE_LENGTH, sizeof(char));

                getline(&line, &line_length, stdin);

                if (sscanf(line, "%d", &num_slaves) != 1) {
                    fprintf(stderr, "Invalid number. Please insert a number!\n");
                    break;
                }
                if (num_slaves <= 0) {
                    fprintf(stderr, "Invalid number. Can't be negative number or 0!\n");
                    break;
                }

                free(line);
            }
            printf("\n");

            // Create slaves
            for (int i = 0; i < num_slaves; ++i) {
                int fork_pid = fork();
                if (fork_pid == 0) {
                    // Child process
                    char *arg_Ptr[3];
                    arg_Ptr[0] = "slave";
                    arg_Ptr[1] = slave_name;
                    arg_Ptr[2] = available_parameters;
                    arg_Ptr[3] = NULL;

                    ret = execv(SLAVE_BIN_PATH, arg_Ptr);
                    if (ret == -1) {
                        fprintf(stderr, "execv() call failed!\n");
                        return RTC_ERROR;
                    }
                } else {
                    // Parent process
                    // fork.pid is slave_pid
                    printf("Started slave process %d with name %s and available parameters %s\n\n", fork_pid, slave_name, available_parameters);
                    registerSlaveLocally(fork_pid);
                }
            }

            sleep(1); // Give time for slaves to start

            // Connect all disconnected slaves
            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.created_slave[i].pid == NO_PID) continue;
                if (self.created_slave[i].connected) continue;
                
                self.control_shmp->request = CONNECT_SLAVE; // Since it's set to AUTOMATIC_TEST
                if (send_connect_slave_request(self.created_slave[i].pid)) {
                    // Failed to send connect request
                    break;
                }

                wait_connect_slave_response(self.created_slave[i].pid);   
            }

            sleep(1); // Give time for slaves to connect
            printf("\n PASSED CONNECTED STAGE!\n\n");

            // Start cycle for all connected slaves
            for (int i = 0; i < MAX_SLAVES; ++i) {
                if (self.shmp->slave_shmseg[i].is_connected) {

                    if (requested_parameters[0] == '1')
                        self.shmp->slave_shmseg[i].requested_parameters |= STRING_PARAMETER_BIT;
                    if (requested_parameters[1] == '1')
                        self.shmp->slave_shmseg[i].requested_parameters |= INT_PARAMETER_BIT;
                    if (requested_parameters[2] == '1')
                        self.shmp->slave_shmseg[i].requested_parameters |= BOOL_PARAMETER_BIT;

                    self.shmp->slave_shmseg[i].communication_cycle_us = communication_cycle_us;

                    self.control_shmp->request = START_SLAVE_CYCLE; // Since it's set to AUTOMATIC_TEST
                    if (send_start_cycle_slave_request(self.shmp->slave_shmseg[i].pid) == RTC_SUCCESS) {
                        wait_start_slave_cycle_response();
                        usleep(650 * 1000); // If they start with same comm cycle at same time we miss signals
                    }
                }
            }
            printf("\n PASSED START CYCLE STAGE!\n\n");

            break;
        }
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
