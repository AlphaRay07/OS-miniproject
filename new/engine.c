/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 * - command-line shape is defined
 * - key runtime data structures are defined
 * - bounded-buffer skeleton is defined
 * - supervisor / client split is outlined
 *
 * Students are expected to design:
 * - the control-plane IPC implementation
 * - container lifecycle and metadata synchronization
 * - clone + namespace setup for each container
 * - producer/consumer behavior for log buffering
 * - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 2048
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef enum {
    TERMINATION_NONE = 0,
    TERMINATION_NORMAL_EXIT,
    TERMINATION_STOPPED,
    TERMINATION_HARD_LIMIT_KILLED,
    TERMINATION_SIGNALLED
} termination_reason_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    termination_reason_t final_reason;
    int stop_requested;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *global_ctx = NULL;

static void supervisor_term_handler(int sig)
{
    (void)sig;
    if (!global_ctx)
        return;

    global_ctx->should_stop = 1;
    if (global_ctx->server_fd >= 0)
        close(global_ctx->server_fd);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s menu\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static const char *reason_to_string(termination_reason_t reason)
{
    switch (reason) {
    case TERMINATION_NONE:
        return "none";
    case TERMINATION_NORMAL_EXIT:
        return "normal_exit";
    case TERMINATION_STOPPED:
        return "stopped";
    case TERMINATION_HARD_LIMIT_KILLED:
        return "hard_limit_killed";
    case TERMINATION_SIGNALLED:
        return "signalled";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 * - block or fail according to your chosen policy when the buffer is full
 * - wake consumers correctly
 * - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 * - wait correctly while the buffer is empty
 * - return a useful status when shutdown is in progress
 * - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 * - remove log chunks from the bounded buffer
 * - route each chunk to the correct per-container log file
 * - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char log_path[PATH_MAX];
    
    mkdir(LOG_DIR, 0777); 
    
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

typedef struct { 
    int read_fd; 
    char container_id[CONTAINER_ID_LEN]; 
    bounded_buffer_t *buffer; 
} pipe_reader_t;

void *pipe_reader_thread(void *arg) {
    pipe_reader_t *pr = (pipe_reader_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;
    
    while ((n = read(pr->read_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        strncpy(item.container_id, pr->container_id, CONTAINER_ID_LEN);
        item.length = n;
        memcpy(item.data, buf, n);
        bounded_buffer_push(pr->buffer, &item);
    }
    close(pr->read_fd);
    free(pr);
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 * - isolated PID / UTS / mount context
 * - chroot or pivot_root into rootfs
 * - working /proc inside container
 * - stdout / stderr redirected to the supervisor logging path
 * - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;
    char *argv[] = {"sh", "-c", config->command, NULL};

    dup2(config->log_write_fd, STDOUT_FILENO);
    dup2(config->log_write_fd, STDERR_FILENO);
    close(config->log_write_fd);

    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot failed");
        return 1;
    }

    mount("proc", "/proc", "proc", 0, NULL);
    execvp(argv[0], argv);
    perror("execvp failed");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static void sigchld_handler(int sig) {
    (void)sig; 
    int status; 
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (global_ctx) {
            pthread_mutex_lock(&global_ctx->metadata_lock);
            container_record_t *curr = global_ctx->containers;
            while (curr) {
                if (curr->host_pid == pid) {
                    if (WIFEXITED(status)) {
                        curr->state = CONTAINER_EXITED;
                        curr->final_reason = curr->stop_requested ? TERMINATION_STOPPED : TERMINATION_NORMAL_EXIT;
                        curr->exit_code = WEXITSTATUS(status);
                        curr->exit_signal = 0;
                    } else if (WIFSIGNALED(status)) {
                        curr->exit_signal = WTERMSIG(status);
                        curr->exit_code = -1;
                        if (curr->stop_requested) {
                            curr->state = CONTAINER_STOPPED;
                            curr->final_reason = TERMINATION_STOPPED;
                        } else if (curr->exit_signal == SIGKILL) {
                            curr->state = CONTAINER_KILLED;
                            curr->final_reason = TERMINATION_HARD_LIMIT_KILLED;
                        } else {
                            curr->state = CONTAINER_KILLED;
                            curr->final_reason = TERMINATION_SIGNALLED;
                        }
                    }
                    curr->host_pid = -1;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&global_ctx->metadata_lock);
        }
    }
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 * - create and bind the control-plane IPC endpoint
 * - initialize shared metadata and the bounded buffer
 * - start the logging thread
 * - accept control requests and update container state
 * - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /*
     * TODO:
     * 1) open /dev/container_monitor
     * 2) create the control socket / FIFO / shared-memory channel
     * 3) install SIGCHLD / SIGINT / SIGTERM handling
     * 4) spawn the logger thread
     * 5) enter the supervisor event loop
     */
    global_ctx = &ctx;
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    
    struct sigaction sa; 
    sa.sa_handler = sigchld_handler; 
    sigemptyset(&sa.sa_mask); 
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    struct sigaction term_sa;
    memset(&term_sa, 0, sizeof(term_sa));
    term_sa.sa_handler = supervisor_term_handler;
    sigemptyset(&term_sa.sa_mask);
    sigaction(SIGINT, &term_sa, NULL);
    sigaction(SIGTERM, &term_sa, NULL);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr; 
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX; 
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    printf("Supervisor running. Listening on %s...\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) continue;
        
        control_request_t req; 
        read(client_fd, &req, sizeof(req));
        control_response_t res; 
        memset(&res, 0, sizeof(res));

        printf("[supervisor] request kind=%d id=%s\n", req.kind, req.container_id);

        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            int pipefd[2]; pipe(pipefd);
            child_config_t *cconf = malloc(sizeof(child_config_t));
            strncpy(cconf->id, req.container_id, CONTAINER_ID_LEN);
            strncpy(cconf->rootfs, req.rootfs, PATH_MAX);
            strncpy(cconf->command, req.command, CHILD_COMMAND_LEN);
            cconf->log_write_fd = pipefd[1];

            char *stack = malloc(STACK_SIZE);
            pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, cconf);
            close(pipefd[1]);

            pipe_reader_t *pr = malloc(sizeof(pipe_reader_t));
            pr->read_fd = pipefd[0]; 
            strncpy(pr->container_id, req.container_id, CONTAINER_ID_LEN); 
            pr->buffer = &ctx.log_buffer;
            
            pthread_t pr_thread; 
            pthread_create(&pr_thread, NULL, pipe_reader_thread, pr); 
            pthread_detach(pr_thread);

            register_with_monitor(ctx.monitor_fd, req.container_id, pid, req.soft_limit_bytes, req.hard_limit_bytes);

            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *rec = malloc(sizeof(container_record_t)); 
            memset(rec, 0, sizeof(*rec));
            strncpy(rec->id, req.container_id, CONTAINER_ID_LEN); 
            rec->host_pid = pid; 
            rec->state = CONTAINER_RUNNING;
            rec->final_reason = TERMINATION_NONE;
            rec->stop_requested = 0;
            rec->soft_limit_bytes = req.soft_limit_bytes;
            rec->hard_limit_bytes = req.hard_limit_bytes;
            rec->started_at = time(NULL);
            snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req.container_id);
            rec->next = ctx.containers; 
            ctx.containers = rec;
            pthread_mutex_unlock(&ctx.metadata_lock);

            if (req.kind == CMD_RUN) {
                container_state_t run_state = CONTAINER_RUNNING;
                termination_reason_t run_reason = TERMINATION_NONE;

                while (run_state == CONTAINER_RUNNING || run_state == CONTAINER_STARTING) {
                    pthread_mutex_lock(&ctx.metadata_lock);
                    container_record_t *walk = ctx.containers;
                    while (walk) {
                        if (strcmp(walk->id, req.container_id) == 0) {
                            run_state = walk->state;
                            run_reason = walk->final_reason;
                            break;
                        }
                        walk = walk->next;
                    }
                    pthread_mutex_unlock(&ctx.metadata_lock);

                    if (run_state == CONTAINER_RUNNING || run_state == CONTAINER_STARTING) {
                        struct timespec nap = {0, 100 * 1000 * 1000};
                        nanosleep(&nap, NULL);
                    }
                }

                snprintf(res.message,
                         sizeof(res.message),
                         "Container finished: state=%s reason=%s",
                         state_to_string(run_state),
                         reason_to_string(run_reason));
                res.status = 0;
            } else {
                res.status = 0;
                strcpy(res.message, "Container started successfully");
            }
        } else if (req.kind == CMD_PS) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *curr = ctx.containers;
            char list[CONTROL_MESSAGE_LEN] = "Containers:\n";
            while(curr) {
                char line[128]; 
                snprintf(line,
                         sizeof(line),
                         "- %s (PID: %d) [%s] reason=%s\n",
                         curr->id,
                         curr->host_pid,
                         state_to_string(curr->state),
                         reason_to_string(curr->final_reason));
                strncat(list, line, CONTROL_MESSAGE_LEN - strlen(list) - 1);
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            res.status = 0; 
            strncpy(res.message, list, CONTROL_MESSAGE_LEN);
        } else if (req.kind == CMD_STOP) {
            pid_t target_pid = -1;
            int found = 0;

            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *curr = ctx.containers;
            while(curr) {
                if(strcmp(curr->id, req.container_id) == 0 && curr->state == CONTAINER_RUNNING) {
                    curr->stop_requested = 1;
                    target_pid = curr->host_pid;
                    found = 1;
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);

            if (!found) {
                res.status = 1;
                strcpy(res.message, "Container not found or not running");
            } else if (kill(target_pid, SIGTERM) != 0) {
                snprintf(res.message,
                         sizeof(res.message),
                         "Failed to send SIGTERM: %s",
                         strerror(errno));
                res.status = 1;
            } else {
                struct timespec grace = {0, 300 * 1000 * 1000};
                nanosleep(&grace, NULL);

                if (kill(target_pid, 0) == 0) {
                    kill(target_pid, SIGKILL);
                    strcpy(res.message, "Stop requested (SIGTERM grace elapsed; SIGKILL sent)");
                } else {
                    strcpy(res.message, "Stop signal sent");
                }
                res.status = 0;
            }
        } else if (req.kind == CMD_LOGS) {
            FILE *fp;
            char path[PATH_MAX];
            size_t n;

            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
            fp = fopen(path, "r");
            if (!fp) {
                res.status = 1;
                snprintf(res.message,
                         sizeof(res.message),
                         "No logs found for container '%s'",
                         req.container_id);
            } else {
                n = fread(res.message, 1, sizeof(res.message) - 1, fp);
                res.message[n] = '\0';
                if (n == 0)
                    snprintf(res.message, sizeof(res.message), "Log file is empty for '%s'", req.container_id);
                fclose(fp);
                res.status = 0;
            }
        }
        write(client_fd, &res, sizeof(res)); 
        close(client_fd);
    }
    
    bounded_buffer_begin_shutdown(&ctx.log_buffer); 
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer); 
    pthread_mutex_destroy(&ctx.metadata_lock); 
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    unlink(CONTROL_PATH);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0); 
    struct sockaddr_un addr; 
    control_response_t res;
    
    memset(&addr, 0, sizeof(addr)); 
    addr.sun_family = AF_UNIX; 
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) { 
        perror("Failed to connect to supervisor"); 
        close(sock); 
        return 1; 
    }
    
    write(sock, req, sizeof(control_request_t)); 
    read(sock, &res, sizeof(control_response_t));
    printf("%s\n", res.message); 
    close(sock); 
    
    return res.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static void prompt_line(const char *prompt, char *out, size_t out_size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, (int)out_size, stdin)) {
        out[0] = '\0';
        return;
    }
    out[strcspn(out, "\n")] = '\0';
}

static int prompt_int_with_default(const char *prompt, int default_value)
{
    char buf[64];
    char *end = NULL;
    long value;

    prompt_line(prompt, buf, sizeof(buf));
    if (buf[0] == '\0')
        return default_value;

    errno = 0;
    value = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0')
        return default_value;

    return (int)value;
}

static int run_menu_cli(void)
{
    char choice[16];

    for (;;) {
        printf("\n=== Runtime CLI Menu ===\n");
        printf("1) start\n");
        printf("2) run\n");
        printf("3) ps\n");
        printf("4) logs\n");
        printf("5) stop\n");
        printf("6) exit\n");
        prompt_line("Select option: ", choice, sizeof(choice));

        if (strcmp(choice, "1") == 0 || strcmp(choice, "2") == 0) {
            control_request_t req;
            char soft_prompt[64];
            char hard_prompt[64];

            memset(&req, 0, sizeof(req));
            req.kind = (strcmp(choice, "1") == 0) ? CMD_START : CMD_RUN;
            req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
            req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

            prompt_line("Container ID: ", req.container_id, sizeof(req.container_id));
            prompt_line("Container rootfs: ", req.rootfs, sizeof(req.rootfs));
            prompt_line("Command (quoted if needed): ", req.command, sizeof(req.command));

            snprintf(soft_prompt,
                     sizeof(soft_prompt),
                     "Soft limit MiB [default %lu]: ",
                     DEFAULT_SOFT_LIMIT >> 20);
            snprintf(hard_prompt,
                     sizeof(hard_prompt),
                     "Hard limit MiB [default %lu]: ",
                     DEFAULT_HARD_LIMIT >> 20);
            req.soft_limit_bytes =
                (unsigned long)prompt_int_with_default(soft_prompt, (int)(DEFAULT_SOFT_LIMIT >> 20)) << 20;
            req.hard_limit_bytes =
                (unsigned long)prompt_int_with_default(hard_prompt, (int)(DEFAULT_HARD_LIMIT >> 20)) << 20;
            req.nice_value = prompt_int_with_default("Nice [-20..19, default 0]: ", 0);

            if (req.container_id[0] == '\0' || req.rootfs[0] == '\0' || req.command[0] == '\0') {
                printf("ID, rootfs, and command are required.\n");
                continue;
            }
            if (req.soft_limit_bytes > req.hard_limit_bytes) {
                printf("Invalid limits: soft limit cannot exceed hard limit.\n");
                continue;
            }
            send_control_request(&req);
            continue;
        }

        if (strcmp(choice, "3") == 0) {
            control_request_t req;
            memset(&req, 0, sizeof(req));
            req.kind = CMD_PS;
            send_control_request(&req);
            continue;
        }

        if (strcmp(choice, "4") == 0) {
            control_request_t req;
            memset(&req, 0, sizeof(req));
            req.kind = CMD_LOGS;
            prompt_line("Container ID for logs: ", req.container_id, sizeof(req.container_id));
            if (req.container_id[0] == '\0') {
                printf("Container ID is required.\n");
                continue;
            }
            send_control_request(&req);
            continue;
        }

        if (strcmp(choice, "5") == 0) {
            control_request_t req;
            memset(&req, 0, sizeof(req));
            req.kind = CMD_STOP;
            prompt_line("Container ID to stop: ", req.container_id, sizeof(req.container_id));
            if (req.container_id[0] == '\0') {
                printf("Container ID is required.\n");
                continue;
            }
            send_control_request(&req);
            continue;
        }

        if (strcmp(choice, "6") == 0)
            return 0;

        printf("Invalid option. Choose 1-6.\n");
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "menu") == 0)
        return run_menu_cli();

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
