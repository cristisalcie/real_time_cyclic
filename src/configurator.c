#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/shm.h>
#include <string.h>
#include <unistd.h>

#include "configurator.h"



static configurator_context_t self = { .shmp = NULL };



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

static int init_control_shared_memory() {
    int shmid;

    // Get or create if not existing shared memory
    shmid = shmget(CONTROL_SHARED_MEMORY_KEY, sizeof(control_shm_t), 0666 | IPC_CREAT);
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

    memset(self.shmp, 0, sizeof(control_shm_t));

    self.shmp->configurator_pid = getpid();
    fprintf(stderr, "Set self.shmp->configurator_pid to %d\n", self.shmp->configurator_pid);

    return RTC_SUCCESS;
}

static void print_main_menu() {
    printf("\n");
    for (int i = 0; i < REQUEST_SIZE; ++i) {
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
    self.shmp->request = req_code;

    free(line);
    printf("\n");
}

static int send_connect_slave_signal() {
    if (kill(self.master_pid, SIGUSR2)) {
        fprintf(stderr, "Failed to send signal SIGUSR2 to master process %d\n", self.master_pid);
        return RTC_ERROR;
    }
    return RTC_SUCCESS;
}

static void wait_connect_slave_response() {
    int err;
    sigset_t sig_set;
    siginfo_t sig_info;

    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR2);

    err = sigwaitinfo(&sig_set, &sig_info);
    if (err == -1) {
        fprintf(stderr, "sigwaitinfo() call failed!");
    } else {
        switch (self.shmp->response)
        {
        case NACK:
            printf("Connect slave request failed!\n");
            break;
        case ACK:
            printf("Connect slave request succeded!\n");
            break;
        default:
            fprintf(stderr, "Unrecognized response received!");
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    int ret;

    if ((ret = block_signals()) != RTC_SUCCESS) return ret;
    if ((ret = init_control_shared_memory()) != RTC_SUCCESS) return ret;   

    while(true) {
        print_main_menu();
        read_req_code();
        switch(self.shmp->request) {
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
            
            int fork_pid = fork();
            if (fork_pid == 0) {
                // Child process
                char *arg_Ptr[3];
                arg_Ptr[0] = "slave";
                arg_Ptr[1] = slave_name;
                arg_Ptr[2] = NULL;

                ret = execv(SLAVE_BIN_PATH, arg_Ptr);
                if (ret == -1) {
                    fprintf(stderr, "execv() call failed!\n");
                }
            } else {
                // Parent process
                // fork.pid is slave_pid
                // TOOD: save slave_pids in self structure to be able to disconnect/delete slave

                printf("Started slave process %d with name %s\n\n", fork_pid, slave_name);
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
            scanf("%d", &slave_pid);
            printf("\n");

            // TOOD: save slave_pids in self structure to be able to disconnect/delete slave
            // TODO: Send correct pid only

            self.shmp->connect_disconnect_slave_pid = slave_pid;
            
            if (send_connect_slave_signal()) {
                // Failed to send connect signal
                break;
            }

            wait_connect_slave_response();
            break;
        }
        case DISCONNECT_SLAVE:
            printf("TODO: Disconnect slave\n");
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
    
    return 0;
}
