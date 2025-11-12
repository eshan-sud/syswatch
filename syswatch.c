/*
 * syswatch.c
 *
 * Multi-threaded system monitor for the assignment.
 *
 * Features:
 * - CPU + Memory monitor thread (every 5s)
 * - Disk monitor thread (every 10s)
 * - Log monitor thread (uses poll() to tail multiple logs; handles rotation)
 * - Network TCP service (status on demand)
 * - Signal handling via dedicated signal thread using sigwait()
 * - Rolling in-memory ring buffer of last N metric samples
 * - Appends metrics to a logfile
 * - Graceful shutdown
 *
 * Build: gcc -std=gnu11 -pthread -o syswatch syswatch.c
 *
 * Basic usage:
 *   ./syswatch -c config.cfg
 *
 * Config format (simple key=value):
 *   LOGFILES=/var/log/syslog,/tmp/test.log
 *   PORT=9999
 *   METRICS_LOG=./metrics.log
 *   RING_SIZE=200
 *
 * Signals:
 *   SIGTERM -> graceful shutdown
 *   SIGUSR1 -> force dump metrics to disk (metrics.log)
 *   SIGHUP  -> reload config file
 *
 * Note: this implementation is simplified for assignment/demo purposes.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdatomic.h>

#define DEFAULT_PORT 9999
#define DEFAULT_METRICS_LOG "./metrics.log"
#define DEFAULT_RING_SIZE 100
#define MAX_LOGFILES 16
#define BUFSZ 4096

typedef struct {
    double cpu_usage;      // percent
    double memory_usage;   // percent used
    double disk_usage;     // percent used (max across partitions)
    time_t timestamp;
} metric_sample_t;

typedef struct {
    metric_sample_t *buf;
    size_t size;
    size_t head; // next write
    size_t count;
    pthread_mutex_t lock;
} ringbuffer_t;

typedef struct {
    double cpu_usage;
    double memory_usage;
    double disk_usage;
    pthread_mutex_t data_lock;
    pthread_cond_t update_cond;
} system_metrics_t;

/* Global state */
static system_metrics_t sys_metrics;
static ringbuffer_t ringbuf;
static atomic_int running = 1;
static char *logfiles[MAX_LOGFILES];
static int n_logfiles = 0;
static int listen_port = DEFAULT_PORT;
static char metrics_logfile[1024] = DEFAULT_METRICS_LOG;
static int ring_size = DEFAULT_RING_SIZE;
static char config_path[1024] = "./syswatch.cfg";

/* forward */
void dump_metrics_to_file();
void reload_config();

static void ring_init(ringbuffer_t *r, size_t size) {
    r->buf = calloc(size, sizeof(metric_sample_t));
    if (!r->buf) {
        fprintf(stderr, "FATAL: cannot allocate ringbuffer of size %zu\n", size);
        exit(1);
    }
    r->size = size;
    r->head = 0;
    r->count = 0;
    pthread_mutex_init(&r->lock, NULL);
}

static void ring_push(ringbuffer_t *r, metric_sample_t *s) {
    pthread_mutex_lock(&r->lock);
    r->buf[r->head] = *s;
    r->head = (r->head + 1) % r->size;
    if (r->count < r->size) r->count++;
    pthread_mutex_unlock(&r->lock);
}

static void ring_snapshot(ringbuffer_t *r, metric_sample_t *out, size_t *out_len) {
    pthread_mutex_lock(&r->lock);
    size_t len = r->count;
    *out_len = len;
    for (size_t i = 0; i < len; i++) {
        size_t idx = (r->head + r->size - len + i) % r->size;
        out[i] = r->buf[idx];
    }
    pthread_mutex_unlock(&r->lock);
}

/* Utilities: read config */
void trim(char *s) {
    // trim leading spaces/tabs
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    // trim trailing whitespace
    char *e = s + strlen(s) - 1;
    while (e >= s && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t')) {
        *e = '\0';
        e--;
    }
}

void parse_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        trim(k);
        trim(v);
        if (strcmp(k, "LOGFILES") == 0) {
            // free previous
            for (int i = 0; i < n_logfiles; ++i) {
                free(logfiles[i]);
                logfiles[i] = NULL;
            }
            // comma separated
            char *tok = strtok(v, ",");
            n_logfiles = 0;
            while (tok && n_logfiles < MAX_LOGFILES) {
                trim(tok);
                logfiles[n_logfiles++] = strdup(tok);
                tok = strtok(NULL, ",");
            }
        } else if (strcmp(k, "PORT") == 0) {
            int p = atoi(v);
            if (p > 0 && p <= 65535) listen_port = p;
        } else if (strcmp(k, "METRICS_LOG") == 0) {
            strncpy(metrics_logfile, v, sizeof(metrics_logfile) - 1);
            metrics_logfile[sizeof(metrics_logfile) - 1] = '\0';
        } else if (strcmp(k, "RING_SIZE") == 0) {
            int rs = atoi(v);
            if (rs > 0) ring_size = rs;
            else ring_size = DEFAULT_RING_SIZE;
        }
    }
    fclose(f);
}

/* CPU usage calculation (reads /proc/stat) */
/* Calculate percent busy between two snapshots */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_times_t;

int read_cpu_times(cpu_times_t *t) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    /* parse first line: cpu  3357 0 4313 1362393 ... */
    sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &t->user, &t->nice, &t->system, &t->idle, &t->iowait, &t->irq, &t->softirq, &t->steal);
    fclose(f);
    return 0;
}

double calc_cpu_usage(cpu_times_t *a, cpu_times_t *b) {
    unsigned long long prev_idle = a->idle + a->iowait;
    unsigned long long idle = b->idle + b->iowait;

    unsigned long long prev_non_idle = a->user + a->nice + a->system + a->irq + a->softirq + a->steal;
    unsigned long long non_idle = b->user + b->nice + b->system + b->irq + b->softirq + b->steal;

    unsigned long long prev_total = prev_idle + prev_non_idle;
    unsigned long long total = idle + non_idle;

    unsigned long long totald = total - prev_total;
    unsigned long long idled = idle - prev_idle;

    if (totald == 0) return 0.0;
    double cpu_percent = (double)(totald - idled) * 100.0 / (double)totald;
    return cpu_percent;
}

/* Memory usage from /proc/meminfo */
double read_memory_usage() {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    unsigned long long mem_total = 0, mem_free = 0, buffers = 0, cached = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %llu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemFree: %llu kB", &mem_free) == 1) continue;
        if (sscanf(line, "Buffers: %llu kB", &buffers) == 1) continue;
        if (sscanf(line, "Cached: %llu kB", &cached) == 1) continue;
    }
    fclose(f);
    if (mem_total == 0) return 0.0;
    unsigned long long used = mem_total - mem_free - buffers - cached;
    double perc = (double)used * 100.0 / (double)mem_total;
    return perc;
}

/* Disk usage across mounted partitions (we will check /proc/mounts & statvfs) */
double read_disk_usage_max() {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return -1;
    char dev[256], mnt[256], type[64], opts[256];
    int freq, passno;
    double maxp = 0.0;
    while (fscanf(f, "%255s %255s %63s %255s %d %d\n", dev, mnt, type, opts, &freq, &passno) == 6) {
        /* skip pseudo filesystems */
        if (strcmp(type, "proc") == 0 || strcmp(type, "sysfs") == 0 || strcmp(type, "tmpfs") == 0 ||
            strcmp(type, "devtmpfs") == 0 || strcmp(type, "devpts") == 0)
            continue;
        struct statvfs st;
        if (statvfs(mnt, &st) == 0) {
            unsigned long long total = st.f_blocks * st.f_frsize;
            unsigned long long free = st.f_bfree * st.f_frsize;
            unsigned long long used = (total > free) ? (total - free) : 0;
            double perc = 0.0;
            if (total > 0) perc = (double)used * 100.0 / (double)total;
            if (perc > maxp) maxp = perc;
        }
    }
    fclose(f);
    return maxp;
}

/* Logging to file (append). */
void append_metrics_log(metric_sample_t *m) {
    FILE *f = fopen(metrics_logfile, "a");
    if (!f) return;
    char timestr[64];
    struct tm tm;
    localtime_r(&m->timestamp, &tm);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s cpu=%.2f mem=%.2f disk=%.2f\n", timestr, m->cpu_usage, m->memory_usage, m->disk_usage);
    fflush(f);
    fclose(f);
}

/* CPU+Memory monitor thread */
void *cpu_mem_thread(void *arg) {
    (void)arg;
    cpu_times_t prev, cur;
    if (read_cpu_times(&prev) != 0) {
        fprintf(stderr, "Failed to read /proc/stat\n");
        memset(&prev, 0, sizeof(prev));
    }
    while (atomic_load(&running)) {
        sleep(5); /* every 5 seconds */
        if (read_cpu_times(&cur) != 0) continue;
        double cpu = calc_cpu_usage(&prev, &cur);
        prev = cur;
        double mem = read_memory_usage();
        double disk = read_disk_usage_max();

        metric_sample_t sample;
        sample.cpu_usage = cpu;
        sample.memory_usage = mem;
        sample.disk_usage = disk;
        sample.timestamp = time(NULL);

        pthread_mutex_lock(&sys_metrics.data_lock);
        sys_metrics.cpu_usage = cpu;
        sys_metrics.memory_usage = mem;
        sys_metrics.disk_usage = disk;
        pthread_cond_broadcast(&sys_metrics.update_cond);
        pthread_mutex_unlock(&sys_metrics.data_lock);

        ring_push(&ringbuf, &sample);
        append_metrics_log(&sample);
    }
    return NULL;
}

/* Disk thread (poll less frequently) */
void *disk_thread(void *arg) {
    (void)arg;
    while (atomic_load(&running)) {
        sleep(10);
        double disk = read_disk_usage_max();
        pthread_mutex_lock(&sys_metrics.data_lock);
        sys_metrics.disk_usage = disk;
        pthread_cond_broadcast(&sys_metrics.update_cond);
        pthread_mutex_unlock(&sys_metrics.data_lock);

        metric_sample_t sample;
        sample.cpu_usage = sys_metrics.cpu_usage;
        sample.memory_usage = sys_metrics.memory_usage;
        sample.disk_usage = disk;
        sample.timestamp = time(NULL);
        ring_push(&ringbuf, &sample);
        append_metrics_log(&sample);
    }
    return NULL;
}

/* Helper: reopen if file rotated: check inode changes */
int open_logfile_follow(const char *path, ino_t *last_inode) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (last_inode) *last_inode = st.st_ino;
    }
    /* seek to end to only capture appended lines */
    lseek(fd, 0, SEEK_END);
    return fd;
}

/* read new lines from fd & check for "error" pattern & react */
void process_log_data(const char *path, int fd) {
    char buf[BUFSZ];
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[r] = '\0';
        /* naive scan: if "error" or "ERROR" appears, print immediate alert */
        if (strcasestr(buf, "error") || strcasestr(buf, "fail")) {
            time_t t = time(NULL);
            char timestr[64];
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);
            fprintf(stderr, "[ALERT %s] Log %s contains error pattern\n", timestr, path);
            FILE *f = fopen(metrics_logfile, "a");
            if (f) {
                fprintf(f, "%s ALERT log=%s contains error pattern\n", timestr, path);
                fclose(f);
            }
        }
    }
}

/* Log monitor thread using poll() */
void *log_monitor_thread(void *arg) {
    (void)arg;
    int fds[MAX_LOGFILES];
    ino_t inodes[MAX_LOGFILES];
    for (int i = 0; i < MAX_LOGFILES; i++) { fds[i] = -1; inodes[i] = 0; }
    while (atomic_load(&running)) {
        int active = 0;
        for (int i = 0; i < n_logfiles; i++) {
            if (fds[i] < 0) {
                fds[i] = open_logfile_follow(logfiles[i], &inodes[i]);
            } else {
                struct stat st;
                if (stat(logfiles[i], &st) == 0) {
                    if (st.st_ino != inodes[i]) {
                        close(fds[i]);
                        fds[i] = open_logfile_follow(logfiles[i], &inodes[i]);
                    }
                }
            }
            if (fds[i] >= 0) active++;
        }
        if (active == 0) {
            sleep(1);
            continue;
        }
        struct pollfd pfd[MAX_LOGFILES];
        int pcount = 0;
        for (int i = 0; i < n_logfiles; i++) {
            if (fds[i] >= 0) {
                pfd[pcount].fd = fds[i];
                pfd[pcount].events = POLLIN | POLLPRI;
                pfd[pcount].revents = 0;
                pcount++;
            }
        }
        int ret = poll(pfd, pcount, 2000);
        if (ret > 0) {
            int idx = 0;
            for (int i = 0; i < n_logfiles; i++) {
                if (fds[i] < 0) continue;
                if (pfd[idx].revents & (POLLIN | POLLPRI)) {
                    process_log_data(logfiles[i], fds[i]);
                }
                idx++;
            }
        } else if (ret == 0) {
            /* timeout */
        } else {
            if (errno == EINTR) continue;
        }
    }
    for (int i = 0; i < n_logfiles; i++) if (fds[i] >= 0) close(fds[i]);
    return NULL;
}

/* TCP network service: allow clients to connect & get JSON status */
void *network_thread(void *arg) {
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return NULL;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return NULL;
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return NULL;
    }
    fd_set readset;
    struct timeval tv;
    while (atomic_load(&running)) {
        FD_ZERO(&readset);
        FD_SET(server_fd, &readset);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int r = select(server_fd + 1, &readset, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(server_fd, &readset)) {
            int c = accept(server_fd, NULL, NULL);
            if (c >= 0) {
                metric_sample_t *snap = calloc(ringbuf.size, sizeof(metric_sample_t));
                if (!snap) { close(c); continue; }
                size_t len = 0;
                ring_snapshot(&ringbuf, snap, &len);

                pthread_mutex_lock(&sys_metrics.data_lock);
                double cpu = sys_metrics.cpu_usage;
                double mem = sys_metrics.memory_usage;
                double disk = sys_metrics.disk_usage;
                pthread_mutex_unlock(&sys_metrics.data_lock);

                char out[8192];
                int offs = snprintf(out, sizeof(out),
                                     "{ \"current\": { \"cpu\": %.2f, \"memory\": %.2f, \"disk\": %.2f }, \"samples\": [",
                                     cpu, mem, disk);
                for (size_t i = 0; i < len; i++) {
                    char ts[64];
                    struct tm tm;
                    localtime_r(&snap[i].timestamp, &tm);
                    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
                    offs += snprintf(out + offs, sizeof(out) - offs,
                                     "{\"timestamp\":\"%s\",\"cpu\":%.2f,\"memory\":%.2f,\"disk\":%.2f}%s",
                                     ts, snap[i].cpu_usage, snap[i].memory_usage, snap[i].disk_usage,
                                     (i + 1 < len) ? "," : "");
                    if (offs > (int)sizeof(out) - 200) break;
                }
                offs += snprintf(out + offs, sizeof(out) - offs, "] }\n");
                send(c, out, strlen(out), 0);
                close(c);
                free(snap);
            }
        } else if (r < 0 && errno != EINTR) {
            /* error */
        }
    }
    close(server_fd);
    return NULL;
}

/* Signal handling thread using sigwait */
void *signal_thread(void *arg) {
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGHUP);

    while (atomic_load(&running)) {
        int sig;
        int r = sigwait(&set, &sig);
        if (r != 0) continue;
        if (sig == SIGTERM) {
            fprintf(stderr, "Received SIGTERM -> shutting down gracefully\n");
            atomic_store(&running, 0);
        } else if (sig == SIGUSR1) {
            fprintf(stderr, "Received SIGUSR1 -> forcing metrics dump\n");
            dump_metrics_to_file();
        } else if (sig == SIGHUP) {
            fprintf(stderr, "Received SIGHUP -> reload config\n");
            reload_config();
        }
    }
    return NULL;
}

/* Dump ring buffer to metrics_logfile now */
void dump_metrics_to_file() {
    metric_sample_t *tmp = calloc(ringbuf.size, sizeof(metric_sample_t));
    if (!tmp) return;
    size_t len = 0;
    ring_snapshot(&ringbuf, tmp, &len);
    FILE *f = fopen(metrics_logfile, "a");
    if (!f) { free(tmp); return; }
    char timestr[64];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s DUMP START (last %zu samples)\n", timestr, len);
    for (size_t i = 0; i < len; i++) {
        struct tm tm2;
        localtime_r(&tmp[i].timestamp, &tm2);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm2);
        fprintf(f, "%s cpu=%.2f mem=%.2f disk=%.2f\n", ts, tmp[i].cpu_usage, tmp[i].memory_usage,
                tmp[i].disk_usage);
    }
    fprintf(f, "%s DUMP END\n", timestr);
    fclose(f);
    free(tmp);
}

/* reload_config callable from SIGHUP handler */
void reload_config() {
    fprintf(stderr, "Reloading config: %s\n", config_path);
    parse_config(config_path);
    /* In this simple implementation we won't resize ring dynamically. */
}

/* Simple usage */
void usage(const char *p) {
    fprintf(stderr, "Usage: %s [-c configfile]\n", p);
}

/* main */
int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                strncpy(config_path, optarg, sizeof(config_path) - 1);
                config_path[sizeof(config_path) - 1] = '\0';
                break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    parse_config(config_path);

    /* init shared metrics */
    sys_metrics.cpu_usage = sys_metrics.memory_usage = sys_metrics.disk_usage = 0.0;
    pthread_mutex_init(&sys_metrics.data_lock, NULL);
    pthread_cond_init(&sys_metrics.update_cond, NULL);

    /* init ring */
    ring_init(&ringbuf, (size_t)ring_size);

    /* block signals in all threads; we'll handle them using sigwait in a dedicated thread */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* create threads */
    pthread_t t_cpu, t_disk, t_log, t_net, t_sig;
    if (pthread_create(&t_cpu, NULL, cpu_mem_thread, NULL) != 0) {
        perror("pthread_create cpu_mem_thread");
        return 1;
    }
    if (pthread_create(&t_disk, NULL, disk_thread, NULL) != 0) {
        perror("pthread_create disk_thread");
        atomic_store(&running, 0);
        pthread_join(t_cpu, NULL);
        return 1;
    }
    if (pthread_create(&t_log, NULL, log_monitor_thread, NULL) != 0) {
        perror("pthread_create log_monitor_thread");
        atomic_store(&running, 0);
        pthread_join(t_cpu, NULL);
        pthread_join(t_disk, NULL);
        return 1;
    }
    if (pthread_create(&t_net, NULL, network_thread, NULL) != 0) {
        perror("pthread_create network_thread");
        atomic_store(&running, 0);
        pthread_join(t_cpu, NULL);
        pthread_join(t_disk, NULL);
        pthread_join(t_log, NULL);
        return 1;
    }
    if (pthread_create(&t_sig, NULL, signal_thread, NULL) != 0) {
        perror("pthread_create signal_thread");
        atomic_store(&running, 0);
        pthread_join(t_cpu, NULL);
        pthread_join(t_disk, NULL);
        pthread_join(t_log, NULL);
        pthread_join(t_net, NULL);
        return 1;
    }

    /* main loop: wait for running -> 0 */
    while (atomic_load(&running)) {
        sleep(1);
    }

    /* join threads & cleanup */
    pthread_join(t_cpu, NULL);
    pthread_join(t_disk, NULL);
    pthread_join(t_log, NULL);
    pthread_join(t_net, NULL);

    /* cancel & join the signal thread (it may be blocked in sigwait) */
    pthread_cancel(t_sig);
    pthread_join(t_sig, NULL);

    /* final dump */
    dump_metrics_to_file();

    /* free duplicated logfile strings */
    for (int i = 0; i < n_logfiles; ++i) {
        free(logfiles[i]);
        logfiles[i] = NULL;
    }

    if (ringbuf.buf) free(ringbuf.buf);

    fprintf(stderr, "SysWatch stopped.\n");
    return 0;
}