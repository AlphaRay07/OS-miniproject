#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>

#include "monitor_shared.h"

#define MAX_CONTAINERS 50

// ---------------- STATE ----------------
typedef enum {
    RUNNING,
    EXITED,
    STOPPED,
    HARD_LIMIT_KILLED
} state_t;

struct container {
    int pid;
    int stop_requested;
    state_t state;
};

struct container containers[MAX_CONTAINERS];
int count = 0;
int fd;

// ---------------- SIGNAL HANDLER ----------------
void cleanup(int sig) {
    printf("\nShutting down supervisor...\n");
    close(fd);
    exit(0);
}

// ---------------- START ----------------
int start_container(char *program) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
        exit(1);
    }

    if (pid == 0) {
        execl(program, program, NULL);
        perror("exec failed");
        exit(1);
    }

    return pid;
}

// ---------------- REGISTER ----------------
void register_with_kernel(int pid, unsigned long soft, unsigned long hard) {
    struct container_config config;

    config.pid = pid;
    config.soft_limit_kb = soft;
    config.hard_limit_kb = hard;

    if (ioctl(fd, REGISTER_CONTAINER, &config) < 0) {
        perror("ioctl failed");
        exit(1);
    }

    printf("Registered PID %d (soft=%lu KB, hard=%lu KB)\n", pid, soft, hard);
}

// ---------------- FIND ----------------
int find_container(int pid) {
    for (int i = 0; i < count; i++) {
        if (containers[i].pid == pid)
            return i;
    }
    return -1;
}

// ---------------- STOP ----------------
void stop_container(int pid) {
    int idx = find_container(pid);

    if (idx == -1) {
        printf("PID not found\n");
        return;
    }

    printf("Stopping PID %d...\n", pid);
    containers[idx].stop_requested = 1;
    kill(pid, SIGTERM);
}

// ---------------- MONITOR ----------------
void monitor_containers() {
    int status;

    for (int i = 0; i < count; i++) {
        if (containers[i].state != RUNNING)
            continue;

        int ret = waitpid(containers[i].pid, &status, WNOHANG);

        if (ret == 0)
            continue; // still running

        if (ret < 0)
            continue; // error

        // -------- CLASSIFICATION --------
        if (WIFEXITED(status)) {
            if (containers[i].stop_requested)
                containers[i].state = STOPPED;
            else
                containers[i].state = EXITED;
        }
        else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);

            if (sig == SIGKILL && !containers[i].stop_requested)
                containers[i].state = HARD_LIMIT_KILLED;
            else
                containers[i].state = STOPPED;
        }
    }
}

// ---------------- PRINT ----------------
void print_status() {
    printf("\n%-10s %-25s\n", "PID", "STATE");
    printf("-----------------------------------\n");

    for (int i = 0; i < count; i++) {
        printf("%-10d ", containers[i].pid);

        switch (containers[i].state) {
            case RUNNING: printf("RUNNING\n"); break;
            case EXITED: printf("EXITED\n"); break;
            case STOPPED: printf("STOPPED\n"); break;
            case HARD_LIMIT_KILLED: printf("HARD_LIMIT_KILLED 💀\n"); break;
        }
    }

    printf("\n(Check dmesg for soft limit warnings)\n");
}

// ---------------- ADD ----------------
void add_container(char *prog, unsigned long soft, unsigned long hard) {
    if (count >= MAX_CONTAINERS) {
        printf("Max container limit reached\n");
        return;
    }

    int pid = start_container(prog);

    containers[count].pid = pid;
    containers[count].stop_requested = 0;
    containers[count].state = RUNNING;

    register_with_kernel(pid, soft, hard);

    count++;
}

// ---------------- MAIN ----------------
int main() {
    signal(SIGINT, cleanup);

    fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }

    char command[256];

    printf("=== Container Supervisor ===\n");

    while (1) {
        monitor_containers();

        printf("\nCommands:\n");
        printf("start <program> <soft_kb> <hard_kb>\n");
        printf("stop <pid>\n");
        printf("ps\n");
        printf("exit\n");
        printf(">> ");

        fgets(command, sizeof(command), stdin);

        // remove newline
        command[strcspn(command, "\n")] = 0;

        // -------- START --------
        if (strncmp(command, "start", 5) == 0) {
            char prog[100];
            unsigned long soft, hard;

            if (sscanf(command, "start %s %lu %lu", prog, &soft, &hard) == 3) {
                add_container(prog, soft, hard);
            } else {
                printf("Usage: start <program> <soft> <hard>\n");
            }
        }

        // -------- STOP --------
        else if (strncmp(command, "stop", 4) == 0) {
            int pid;
            if (sscanf(command, "stop %d", &pid) == 1) {
                stop_container(pid);
            } else {
                printf("Usage: stop <pid>\n");
            }
        }

        // -------- PS --------
        else if (strcmp(command, "ps") == 0) {
            print_status();
        }

        // -------- EXIT --------
        else if (strcmp(command, "exit") == 0) {
            cleanup(0);
        }

        else {
            printf("Unknown command\n");
        }

        sleep(1);
    }

    return 0;
}