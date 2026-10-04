/*
 * agent_816.c — RemoteOps Agent
 * Registration number: IT24100816
 * Personalised port:   9410
 * SID tag:             6180
 * Auth token:          OPS-0816
 * Log file:            remoteops_IT24100816.log
 * Storage path:        ./agentfiles/IT24100816/
 *
 * Concurrency: thread-per-connection (detached pthreads).
 * A single mutex protects the shared log file.
 * Each session may have at most one active UDP monitor thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/utsname.h>

#define AGENT_PORT       9410
#define SID_TAG          "6180"
#define AUTH_TOKEN       "OPS-0816"
#define BACKLOG          10
#define MAX_LINE         4096
#define LOG_FILE         "remoteops_IT24100816.log"
#define STORAGE_DIR      "./agentfiles/IT24100816"
#define MAX_FILE_SIZE    (10 * 1024 * 1024)   /* 10 MB cap for ERR 004 */

/* ---------- shared log ---------- */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_fp = NULL;

static void log_event(const char *fmt, ...) {
    if (!log_fp) return;
    pthread_mutex_lock(&log_mutex);

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(log_fp, "[%s] ", ts);
    va_list ap; va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', log_fp);
    fflush(log_fp);

    printf("[agent][%s] ", ts);
    va_list ap2; va_start(ap2, fmt);
    vprintf(fmt, ap2);
    va_end(ap2);
    putchar('\n');
    fflush(stdout);

    pthread_mutex_unlock(&log_mutex);
}

/* ---------- per-connection buffered reader + monitor state ---------- */
typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
    unsigned char buf[8192];
    size_t buf_len;
    size_t buf_pos;

    volatile int monitor_active;   /* 1 while a monitor thread runs */
    int          udp_port;         /* controller's requested UDP port */
} session_t;

static int fill_buffer(session_t *s) {
    if (s->buf_pos < s->buf_len) return 1;
    ssize_t n = recv(s->fd, s->buf, sizeof(s->buf), 0);
    if (n <= 0) return 0;
    s->buf_len = (size_t)n;
    s->buf_pos = 0;
    return 1;
}

static ssize_t sread_line(session_t *s, char *out, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen - 1) {
        if (!fill_buffer(s)) return -1;
        unsigned char c = s->buf[s->buf_pos++];
        if (c == '\n') break;
        out[i++] = (char)c;
    }
    out[i] = '\0';
    return (ssize_t)i;
}

static int sread_exact(session_t *s, unsigned char *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (!fill_buffer(s)) return -1;
        size_t avail = s->buf_len - s->buf_pos;
        size_t take  = (avail < n - got) ? avail : (n - got);
        memcpy(out + got, s->buf + s->buf_pos, take);
        s->buf_pos += take;
        got         += take;
    }
    return 0;
}

/* ---------- output helpers ---------- */
static int send_line(int fd, const char *str) {
    size_t len = strlen(str);
    if (send(fd, str, len, 0) != (ssize_t)len) return -1;
    if (send(fd, "\n", 1, 0) != 1)              return -1;
    return 0;
}

static int send_exact(int fd, const unsigned char *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t k = send(fd, buf + sent, n - sent, 0);
        if (k <= 0) return -1;
        sent += (size_t)k;
    }
    return 0;
}

/* ---------- command implementations ---------- */

static void run_cmd_first_line(const char *cmd, char *out, size_t outlen) {
    FILE *fp = popen(cmd, "r");
    out[0] = '\0';
    if (!fp) return;
    if (fgets(out, outlen, fp)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = '\0';
    }
    pclose(fp);
}

static int build_sysinfo(char *out, size_t outlen) {
    double cpu = 0.0;
    FILE *fp = fopen("/proc/loadavg", "r");
    if (fp) { fscanf(fp, "%lf", &cpu); fclose(fp); }

    long mem_total_kb = 0, mem_avail_kb = 0;
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char key[64], unit[16]; long val;
        while (fscanf(fp, "%63s %ld %15s", key, &val, unit) == 3) {
            if (!strcmp(key, "MemTotal:"))     mem_total_kb = val;
            if (!strcmp(key, "MemAvailable:")) mem_avail_kb = val;
            if (mem_total_kb && mem_avail_kb) break;
        }
        fclose(fp);
    }
    long mem_used_mb = (mem_total_kb - mem_avail_kb) / 1024;

    double uptime = 0.0;
    fp = fopen("/proc/uptime", "r");
    if (fp) { fscanf(fp, "%lf", &uptime); fclose(fp); }

    return snprintf(out, outlen,
                    "OK SYSINFO %.2f %ld %ld SID:%s",
                    cpu, mem_used_mb, (long)uptime, SID_TAG);
}

static int build_listproc(char *out, size_t outlen) {
    FILE *fp = popen("ps -eo comm --no-headers", "r");
    int written = snprintf(out, outlen, "OK PROCs ");
    if (!fp) {
        written += snprintf(out + written, outlen - written, " SID:%s", SID_TAG);
        return written;
    }
    char line[128]; int first = 1;
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        if (written + (int)n + 16 >= (int)outlen) {
            written += snprintf(out + written, outlen - written, ",...");
            break;
        }
        written += snprintf(out + written, outlen - written,
                            "%s%s", first ? "" : ",", line);
        first = 0;
    }
    pclose(fp);
    written += snprintf(out + written, outlen - written, " SID:%s", SID_TAG);
    return written;
}

static int build_exec(const char *name, char *out, size_t outlen) {
    const char *cmd = NULL;
    if      (!strcmp(name, "DATE"))     cmd = "date";
    else if (!strcmp(name, "UPTIME"))   cmd = "uptime";
    else if (!strcmp(name, "DISKFREE")) cmd = "df -h /";
    else if (!strcmp(name, "HOSTNAME")) cmd = "hostname";
    else if (!strcmp(name, "WHOAMI"))   cmd = "whoami";
    else return snprintf(out, outlen,
                         "ERR 002 COMMAND_NOT_ALLOWED SID:%s", SID_TAG);
    char result[1024];
    run_cmd_first_line(cmd, result, sizeof(result));
    return snprintf(out, outlen,
                    "OK EXEC_RESULT %s SID:%s", result, SID_TAG);
}

/* ---------- PUT / GET ---------- */

static int is_safe_filename(const char *name) {
    if (!name || !*name) return 0;
    if (strchr(name, '/'))  return 0;
    if (strstr(name, "..")) return 0;
    return 1;
}

static void handle_put(session_t *s, const char *filename, long filesize,
                       const char *ip) {
    char resp[512];

    if (!is_safe_filename(filename)) {
        snprintf(resp, sizeof(resp),
                 "ERR 003 BAD_FILENAME SID:%s", SID_TAG);
        send_line(s->fd, resp);
        log_event("PUT rejected (bad filename) from %s: %s", ip, filename);
        return;
    }
    if (filesize < 0 || filesize > MAX_FILE_SIZE) {
        snprintf(resp, sizeof(resp),
                 "ERR 004 FILE_TOO_LARGE SID:%s", SID_TAG);
        send_line(s->fd, resp);
        log_event("PUT rejected (size %ld) from %s", filesize, ip);
        unsigned char drain[4096];
        long remaining = filesize;
        while (remaining > 0) {
            size_t chunk = (remaining > (long)sizeof(drain)) ?
                            sizeof(drain) : (size_t)remaining;
            if (sread_exact(s, drain, chunk) != 0) break;
            remaining -= (long)chunk;
        }
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, filename);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        snprintf(resp, sizeof(resp),
                 "ERR 006 FILE_WRITE_FAILED SID:%s", SID_TAG);
        send_line(s->fd, resp);
        log_event("PUT failed (fopen %s) from %s", path, ip);
        unsigned char drain[4096];
        long remaining = filesize;
        while (remaining > 0) {
            size_t chunk = (remaining > (long)sizeof(drain)) ?
                            sizeof(drain) : (size_t)remaining;
            if (sread_exact(s, drain, chunk) != 0) break;
            remaining -= (long)chunk;
        }
        return;
    }

    unsigned char chunk[4096];
    long remaining = filesize;
    while (remaining > 0) {
        size_t take = (remaining > (long)sizeof(chunk)) ?
                       sizeof(chunk) : (size_t)remaining;
        if (sread_exact(s, chunk, take) != 0) {
            log_event("PUT aborted mid-transfer from %s (recv error)", ip);
            fclose(fp);
            return;
        }
        if (fwrite(chunk, 1, take, fp) != take) {
            log_event("PUT aborted mid-transfer from %s (write error)", ip);
            fclose(fp);
            return;
        }
        remaining -= (long)take;
    }
    fclose(fp);

    snprintf(resp, sizeof(resp),
             "OK FILE_RECEIVED %s SID:%s", filename, SID_TAG);
    send_line(s->fd, resp);
    log_event("PUT OK from %s: %s (%ld bytes)", ip, filename, filesize);
}

static void handle_get(session_t *s, const char *filename, const char *ip) {
    char resp[512];

    if (!is_safe_filename(filename)) {
        snprintf(resp, sizeof(resp),
                 "ERR 005 FILE_NOT_FOUND SID:%s", SID_TAG);
        send_line(s->fd, resp);
        log_event("GET rejected (bad filename) from %s: %s", ip, filename);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, filename);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(resp, sizeof(resp),
                 "ERR 005 FILE_NOT_FOUND SID:%s", SID_TAG);
        send_line(s->fd, resp);
        log_event("GET miss from %s: %s", ip, filename);
        return;
    }

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return; }
    long filesize = ftell(fp);
    if (filesize < 0) { fclose(fp); return; }
    rewind(fp);

    snprintf(resp, sizeof(resp),
             "OK FILE_SEND %s %ld SID:%s", filename, filesize, SID_TAG);
    send_line(s->fd, resp);

    unsigned char chunk[4096];
    long remaining = filesize;
    while (remaining > 0) {
        size_t take = (remaining > (long)sizeof(chunk)) ?
                       sizeof(chunk) : (size_t)remaining;
        size_t got = fread(chunk, 1, take, fp);
        if (got == 0) break;
        if (send_exact(s->fd, chunk, got) != 0) {
            log_event("GET aborted mid-transfer to %s", ip);
            fclose(fp);
            return;
        }
        remaining -= (long)got;
    }
    fclose(fp);
    log_event("GET OK to %s: %s (%ld bytes)", ip, filename, filesize);
}

/* ---------- UDP monitor thread ---------- */

static void *monitor_thread(void *arg) {
    session_t *s = (session_t *)arg;

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        log_event("MONITOR: socket() failed for %s", s->ip);
        s->monitor_active = 0;
        return NULL;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(s->udp_port);
    inet_pton(AF_INET, s->ip, &dst.sin_addr);

    log_event("MONITOR started: udp -> %s:%d", s->ip, s->udp_port);

    while (s->monitor_active) {
        char payload[256];
        char sysbuf[256];
        build_sysinfo(sysbuf, sizeof(sysbuf));
        /* strip the leading "OK " and append '\n' so receivers can
         * display one datagram per line (nc -u -l has no framing). */
        snprintf(payload, sizeof(payload), "%s\n", sysbuf + 3);

        sendto(udp_fd, payload, strlen(payload), 0,
               (struct sockaddr *)&dst, sizeof(dst));

        for (int i = 0; i < 10 && s->monitor_active; i++) {
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }

    close(udp_fd);
    log_event("MONITOR stopped for %s:%d", s->ip, s->udp_port);
    return NULL;
}

/* ---------- per-client thread ---------- */

static void *handle_client(void *arg) {
    session_t *s = (session_t *)arg;
    char ip[INET_ADDRSTRLEN];
    strncpy(ip, s->ip, sizeof(ip));

    log_event("CONNECT from %s (fd=%d)", ip, s->fd);

    int authenticated = 0;
    char line[MAX_LINE];
    char resp[4096];

    while (1) {
        ssize_t n = sread_line(s, line, sizeof(line));
        if (n < 0) {
            log_event("DISCONNECT from %s (fd=%d) %s",
                      ip, s->fd, authenticated ? "[authed]" : "[unauthed]");
            break;
        }

        char verb[64]      = {0};
        char arg1[MAX_LINE] = {0};
        char arg2[64]      = {0};
        sscanf(line, "%63s %4095s %63s", verb, arg1, arg2);

        log_event("CMD from %s: %s", ip, line);

        if (!strcmp(verb, "AUTH")) {
            if (!strcmp(arg1, AUTH_TOKEN)) {
                authenticated = 1;
                snprintf(resp, sizeof(resp),
                         "OK AUTHENTICATED SID:%s", SID_TAG);
                send_line(s->fd, resp);
                log_event("AUTH OK from %s", ip);
            } else {
                snprintf(resp, sizeof(resp),
                         "ERR 001 AUTH_FAILED SID:%s", SID_TAG);
                send_line(s->fd, resp);
                log_event("AUTH FAILED from %s", ip);
            }
        }
        else if (!authenticated) {
            snprintf(resp, sizeof(resp),
                     "ERR 001 AUTH_REQUIRED SID:%s", SID_TAG);
            send_line(s->fd, resp);
        }
        else if (!strcmp(verb, "QUIT")) {
            snprintf(resp, sizeof(resp), "OK BYE SID:%s", SID_TAG);
            send_line(s->fd, resp);
            log_event("QUIT from %s", ip);
            break;
        }
        else if (!strcmp(verb, "SYSINFO")) {
            build_sysinfo(resp, sizeof(resp));
            send_line(s->fd, resp);
        }
        else if (!strcmp(verb, "LISTPROC")) {
            build_listproc(resp, sizeof(resp));
            send_line(s->fd, resp);
        }
        else if (!strcmp(verb, "EXEC")) {
            build_exec(arg1, resp, sizeof(resp));
            send_line(s->fd, resp);
        }
        else if (!strcmp(verb, "PUT")) {
            long filesize = atol(arg2);
            handle_put(s, arg1, filesize, ip);
        }
        else if (!strcmp(verb, "GET")) {
            handle_get(s, arg1, ip);
        }
        else if (!strcmp(verb, "MONITOR")) {
            if (!strcmp(arg1, "START")) {
                if (s->monitor_active) {
                    snprintf(resp, sizeof(resp),
                             "ERR 007 ALREADY_MONITORING SID:%s", SID_TAG);
                    send_line(s->fd, resp);
                } else {
                    int port = atoi(arg2);
                    if (port <= 0 || port > 65535) {
                        snprintf(resp, sizeof(resp),
                                 "ERR 008 BAD_UDP_PORT SID:%s", SID_TAG);
                        send_line(s->fd, resp);
                    } else {
                        s->udp_port       = port;
                        s->monitor_active = 1;
                        pthread_t mtid;
                        if (pthread_create(&mtid, NULL,
                                           monitor_thread, s) == 0) {
                            pthread_detach(mtid);
                            snprintf(resp, sizeof(resp),
                                     "OK MONITOR_STARTED SID:%s", SID_TAG);
                            send_line(s->fd, resp);
                        } else {
                            s->monitor_active = 0;
                            snprintf(resp, sizeof(resp),
                                     "ERR 009 MONITOR_FAILED SID:%s", SID_TAG);
                            send_line(s->fd, resp);
                        }
                    }
                }
            }
            else if (!strcmp(arg1, "STOP")) {
                s->monitor_active = 0;
                snprintf(resp, sizeof(resp),
                         "OK MONITOR_STOPPED SID:%s", SID_TAG);
                send_line(s->fd, resp);
            }
            else {
                snprintf(resp, sizeof(resp),
                         "ERR 002 COMMAND_NOT_ALLOWED SID:%s", SID_TAG);
                send_line(s->fd, resp);
            }
        }
        else {
            snprintf(resp, sizeof(resp),
                     "ERR 002 COMMAND_NOT_ALLOWED SID:%s", SID_TAG);
            send_line(s->fd, resp);
        }
    }

    /* Signal any monitor thread to stop. Do NOT free(s) — the monitor
     * thread may still be reading s->monitor_active / s->udp_port.
     * ~8 KB leak per connection; documented in design diary. */
    s->monitor_active = 0;
    close(s->fd);
    return NULL;
}

/* ---------- main ---------- */

int main(void) {
    /* Ensure the personalised storage directory exists */
    mkdir("./agentfiles", 0755);
    mkdir(STORAGE_DIR, 0755);

    log_fp = fopen(LOG_FILE, "a");
    if (!log_fp) { perror("fopen log"); exit(1); }
    log_event("=== AGENT START (pid=%d) listening on port %d ===",
              getpid(), AGENT_PORT);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(AGENT_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(listen_fd, BACKLOG) < 0) { perror("listen"); exit(1); }

    printf("[agent] listening on port %d\n", AGENT_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        session_t *s = calloc(1, sizeof(session_t));
        if (!s) { close(client_fd); continue; }
        s->fd      = client_fd;
        s->buf_len = 0;
        s->buf_pos = 0;
        s->monitor_active = 0;
        s->udp_port       = 0;
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  s->ip, sizeof(s->ip));

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, s) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(s);
            continue;
        }
        pthread_detach(tid);
    }

    close(listen_fd);
    fclose(log_fp);
    return 0;
}
