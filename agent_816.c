/*
 * agent_816.c — RemoteOps Agent
 * Registration number: IT24100816
 * Personalised port:   9410
 * SID tag:             6180
 * Auth token:          OPS-0816
 * Log file:            remoteops_IT24100816.log
 * Storage path:        ./agentfiles/IT24100816/
 *
 * Concurrency model: thread-per-connection (detached pthreads).
 * A single mutex protects the shared log file.
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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/utsname.h>

#define AGENT_PORT   9410
#define SID_TAG      "6180"
#define AUTH_TOKEN   "OPS-0816"
#define BACKLOG      10
#define MAX_LINE     4096
#define LOG_FILE     "remoteops_IT24100816.log"

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

    /* Write to log file */
    fprintf(log_fp, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', log_fp);
    fflush(log_fp);

    /* ALSO echo to console so we can see events live during dev.
     * Copy va_list because vfprintf consumes it. */
    printf("[agent][%s] ", ts);
    va_list ap2;
    va_start(ap2, fmt);
    vprintf(fmt, ap2);
    va_end(ap2);
    putchar('\n');
    fflush(stdout);

    pthread_mutex_unlock(&log_mutex);
}
/* Need stdarg for va_list */
#include <stdarg.h>

/* ---------- helpers ---------- */

static ssize_t read_line(int fd, char *buf, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

static int send_line(int fd, const char *str) {
    size_t len = strlen(str);
    if (send(fd, str, len, 0) != (ssize_t)len) return -1;
    if (send(fd, "\n", 1, 0) != 1)              return -1;
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
        char key[64], unit[16];
        long val;
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

    char line[128];
    int first = 1;
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
    else {
        return snprintf(out, outlen,
                        "ERR 002 COMMAND_NOT_ALLOWED SID:%s", SID_TAG);
    }
    char result[1024];
    run_cmd_first_line(cmd, result, sizeof(result));
    return snprintf(out, outlen,
                    "OK EXEC_RESULT %s SID:%s", result, SID_TAG);
}

/* ---------- per-client session (thread entry) ---------- */

typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
} session_t;

static void *handle_client(void *arg) {
    session_t *s = (session_t *)arg;
    int  fd = s->fd;
    char ip[INET_ADDRSTRLEN];
    strncpy(ip, s->ip, sizeof(ip));
    free(s);

    log_event("CONNECT from %s (fd=%d)", ip, fd);

    int authenticated = 0;
    char line[MAX_LINE];
    char resp[4096];

    while (1) {
        ssize_t n = read_line(fd, line, sizeof(line));
        if (n < 0) {
            log_event("DISCONNECT from %s (fd=%d) %s",
                      ip, fd, authenticated ? "[authed]" : "[unauthed]");
            break;
        }

        char verb[64] = {0};
        char arg[MAX_LINE] = {0};
        sscanf(line, "%63s %4095[^\n]", verb, arg);

        log_event("CMD from %s: %s", ip, line);

        if (!strcmp(verb, "AUTH")) {
            if (!strcmp(arg, AUTH_TOKEN)) {
                authenticated = 1;
                snprintf(resp, sizeof(resp),
                         "OK AUTHENTICATED SID:%s", SID_TAG);
                send_line(fd, resp);
                log_event("AUTH OK from %s", ip);
            } else {
                snprintf(resp, sizeof(resp),
                         "ERR 001 AUTH_FAILED SID:%s", SID_TAG);
                send_line(fd, resp);
                log_event("AUTH FAILED from %s", ip);
            }
        }
        else if (!authenticated) {
            snprintf(resp, sizeof(resp),
                     "ERR 001 AUTH_REQUIRED SID:%s", SID_TAG);
            send_line(fd, resp);
        }
        else if (!strcmp(verb, "QUIT")) {
            snprintf(resp, sizeof(resp), "OK BYE SID:%s", SID_TAG);
            send_line(fd, resp);
            log_event("QUIT from %s", ip);
            break;
        }
        else if (!strcmp(verb, "SYSINFO")) {
            build_sysinfo(resp, sizeof(resp));
            send_line(fd, resp);
        }
        else if (!strcmp(verb, "LISTPROC")) {
            build_listproc(resp, sizeof(resp));
            send_line(fd, resp);
        }
        else if (!strcmp(verb, "EXEC")) {
            build_exec(arg, resp, sizeof(resp));
            send_line(fd, resp);
        }
        else {
            snprintf(resp, sizeof(resp),
                     "ERR 002 COMMAND_NOT_ALLOWED SID:%s", SID_TAG);
            send_line(fd, resp);
        }
    }

    close(fd);
    return NULL;
}

/* ---------- main ---------- */

int main(void) {
    /* Open the personalised log file */
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

        session_t *s = malloc(sizeof(session_t));
        if (!s) { close(client_fd); continue; }
        s->fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  s->ip, sizeof(s->ip));

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, s) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(s);
            continue;
        }
        pthread_detach(tid);   /* we don't join — OS reclaims on exit */
    }

    close(listen_fd);
    fclose(log_fp);
    return 0;
}
