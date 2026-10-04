/*
 * agent_816.c — RemoteOps Agent
 * Registration number: IT24100816
 * Personalised port:   9410
 * SID tag:             6180
 * Auth token:          OPS-0816
 * Log file:            remoteops_IT24100816.log
 * Storage path:        ./agentfiles/IT24100816/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/utsname.h>

#define AGENT_PORT   9410
#define SID_TAG      "6180"
#define AUTH_TOKEN   "OPS-0816"
#define BACKLOG      10
#define MAX_LINE     4096

/* -------- helpers -------- */

/* Read one line (terminated by '\n') from fd into buf.
 * Returns the number of chars read (excluding '\n'), or -1 on EOF/error. */
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

/* Send str followed by '\n'. Returns 0 on success, -1 on error. */
static int send_line(int fd, const char *str) {
    size_t len = strlen(str);
    if (send(fd, str, len, 0) != (ssize_t)len) return -1;
    if (send(fd, "\n", 1, 0) != 1)              return -1;
    return 0;
}

/* -------- command implementations -------- */

/* Run a fixed shell command, capture its first line of output into out.
 * IMPORTANT: cmd must be a hardcoded literal — never user input. */
static void run_cmd_first_line(const char *cmd, char *out, size_t outlen) {
    FILE *fp = popen(cmd, "r");
    out[0] = '\0';
    if (!fp) return;
    if (fgets(out, outlen, fp)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r')) {
            out[--n] = '\0';
        }
    }
    pclose(fp);
}

/* SYSINFO: read /proc/loadavg, /proc/meminfo, /proc/uptime */
static int build_sysinfo(char *out, size_t outlen) {
    double cpu = 0.0;
    FILE *fp = fopen("/proc/loadavg", "r");
    if (fp) { fscanf(fp, "%lf", &cpu); fclose(fp); }

    long mem_total_kb = 0, mem_avail_kb = 0;
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char key[64];
        long val;
        char unit[16];
        while (fscanf(fp, "%63s %ld %15s", key, &val, unit) == 3) {
            if (strcmp(key, "MemTotal:") == 0)    mem_total_kb = val;
            if (strcmp(key, "MemAvailable:") == 0) mem_avail_kb = val;
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

/* LISTPROC: run `ps -eo comm --no-headers`, join names with commas */
static int build_listproc(char *out, size_t outlen) {
    FILE *fp = popen("ps -eo comm --no-headers", "r");
    if (!fp) {
        return snprintf(out, outlen,
                        "OK PROCs  SID:%s", SID_TAG);
    }

    char line[128];
    int written = snprintf(out, outlen, "OK PROCs ");
    int first = 1;
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) continue;
        /* reserve space for " SID:6180" suffix */
        if (written + (int)n + 16 >= (int)outlen) {
            written += snprintf(out + written, outlen - written, ",...");
            break;
        }
        written += snprintf(out + written, outlen - written,
                            "%s%s", first ? "" : ",", line);
        first = 0;
    }
    pclose(fp);
    written += snprintf(out + written, outlen - written,
                        " SID:%s", SID_TAG);
    return written;
}

/* EXEC: hardcoded whitelist — never pass user input to a shell */
static int build_exec(const char *name, char *out, size_t outlen) {
    const char *cmd = NULL;
    if      (strcmp(name, "DATE")     == 0) cmd = "date";
    else if (strcmp(name, "UPTIME")   == 0) cmd = "uptime";
    else if (strcmp(name, "DISKFREE") == 0) cmd = "df -h /";
    else if (strcmp(name, "HOSTNAME") == 0) cmd = "hostname";
    else if (strcmp(name, "WHOAMI")   == 0) cmd = "whoami";
    else {
        return snprintf(out, outlen,
                        "ERR 002 COMMAND_NOT_ALLOWED SID:%s", SID_TAG);
    }

    char result[1024];
    run_cmd_first_line(cmd, result, sizeof(result));
    return snprintf(out, outlen,
                    "OK EXEC_RESULT %s SID:%s", result, SID_TAG);
}

/* -------- main -------- */

int main(void) {
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
    printf("[agent] listening on port %d\n", AGENT_PORT);

    if (listen(listen_fd, BACKLOG) < 0) { perror("listen"); exit(1); }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) { perror("accept"); exit(1); }

    printf("[agent] client connected from %s\n",
           inet_ntoa(client_addr.sin_addr));

    /* Unauthenticated for now — Step C makes this real. */
    int authenticated = 0;

    char line[MAX_LINE];
    while (1) {
        ssize_t n = read_line(client_fd, line, sizeof(line));
        if (n < 0) {
            printf("[agent] client disconnected\n");
            break;
        }

        printf("[agent] recv: \"%s\"\n", line);

        /* Split into verb + argument (first whitespace) */
        char verb[64] = {0};
        char arg[MAX_LINE] = {0};
        sscanf(line, "%63s %4095[^\n]", verb, arg);

        /* --- command dispatch --- */
        if (strcmp(verb, "AUTH") == 0) {
            if (strcmp(arg, AUTH_TOKEN) == 0) {
                authenticated = 1;
                printf("[agent] AUTH ok from %s\n",
                       inet_ntoa(client_addr.sin_addr));
                char resp[128];
                snprintf(resp, sizeof(resp),
                         "OK AUTHENTICATED SID:%s", SID_TAG);
                send_line(client_fd, resp);
            } else {
                printf("[agent] AUTH failed (bad token)\n");
                char resp[128];
                snprintf(resp, sizeof(resp),
                         "ERR 001 AUTH_FAILED SID:%s", SID_TAG);
                send_line(client_fd, resp);
            }
        }
        else if (!authenticated) {
            /* Every other command before AUTH succeeds → reject */
            char resp[128];
            snprintf(resp, sizeof(resp),
                     "ERR 001 AUTH_REQUIRED SID:%s", SID_TAG);
            send_line(client_fd, resp);
        }
        else if (strcmp(verb, "QUIT") == 0) {
            char resp[128];
            snprintf(resp, sizeof(resp),
                     "OK BYE SID:%s", SID_TAG);
            send_line(client_fd, resp);
            printf("[agent] client requested QUIT, closing\n");
            break;
        }
        else if (strcmp(verb, "SYSINFO") == 0) {
            char resp[512];
            build_sysinfo(resp, sizeof(resp));
            send_line(client_fd, resp);
        }
        else if (strcmp(verb, "LISTPROC") == 0) {
            char resp[4096];
            build_listproc(resp, sizeof(resp));
            send_line(client_fd, resp);
        }
        else if (strcmp(verb, "EXEC") == 0) {
            char resp[2048];
            build_exec(arg, resp, sizeof(resp));
            send_line(client_fd, resp);
        }
        else {
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "ERR 002 COMMAND_NOT_ALLOWED SID:%s", SID_TAG);
            send_line(client_fd, resp);
        }
    }

    close(client_fd);
    close(listen_fd);
    printf("[agent] shutting down\n");
    return 0;
}
