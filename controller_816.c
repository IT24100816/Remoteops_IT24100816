/*
 * controller_816.c — RemoteOps Controller
 * Registration number: IT24100816
 * Agent default:       127.0.0.1:9410
 * SID tag (for display): 6180
 *
 * Interactive TCP client. Sends AUTH, SYSINFO, LISTPROC, EXEC, PUT,
 * GET, MONITOR START/STOP, QUIT. For MONITOR START it spawns a UDP
 * listener thread that prints incoming datagrams.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_AGENT_IP   "127.0.0.1"
#define AGENT_PORT         9410
#define SID_TAG            "6180"
#define MAX_LINE           4096
#define DOWNLOAD_DIR       "./downloads"

/* ---------- TCP buffered reader (same design as the Agent) ---------- */
typedef struct {
    int  fd;
    unsigned char buf[8192];
    size_t buf_len;
    size_t buf_pos;
} reader_t;

static int fill_buffer(reader_t *r) {
    if (r->buf_pos < r->buf_len) return 1;
    ssize_t n = recv(r->fd, r->buf, sizeof(r->buf), 0);
    if (n <= 0) return 0;
    r->buf_len = (size_t)n;
    r->buf_pos = 0;
    return 1;
}

static ssize_t read_line(reader_t *r, char *out, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen - 1) {
        if (!fill_buffer(r)) return -1;
        unsigned char c = r->buf[r->buf_pos++];
        if (c == '\n') break;
        out[i++] = (char)c;
    }
    out[i] = '\0';
    return (ssize_t)i;
}

static int read_exact(reader_t *r, unsigned char *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (!fill_buffer(r)) return -1;
        size_t avail = r->buf_len - r->buf_pos;
        size_t take  = (avail < n - got) ? avail : (n - got);
        memcpy(out + got, r->buf + r->buf_pos, take);
        r->buf_pos += take;
        got         += take;
    }
    return 0;
}

/* ---------- output helpers ---------- */
static int send_all(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    const unsigned char *p = buf;
    while (sent < n) {
        ssize_t k = send(fd, p + sent, n - sent, 0);
        if (k <= 0) return -1;
        sent += (size_t)k;
    }
    return 0;
}

/* ---------- UDP listener thread ---------- */
typedef struct {
    int  udp_port;
    volatile int *running;   /* points to a shared int in main */
} udp_args_t;

static void *udp_listener(void *arg) {
    udp_args_t *a = (udp_args_t *)arg;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("udp socket"); free(a); return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(a->udp_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("udp bind");
        close(fd);
        free(a);
        return NULL;
    }

    printf("  [UDP listener started on 0.0.0.0:%d]\n", a->udp_port);
    fflush(stdout);

    struct timeval tv = { 0, 200000 };  /* 200 ms so we can check *running */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (*(a->running)) {
        char buf[2048];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&src, &srclen);
        if (n < 0) continue;   /* timeout → loop and re-check running */
        buf[n] = '\0';
        /* strip trailing newline for display */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        printf("  [UDP] %s\n", buf);
        fflush(stdout);
    }

    close(fd);
    printf("  [UDP listener stopped]\n");
    fflush(stdout);
    free(a);
    return NULL;
}

/* ---------- command handlers ---------- */

static int do_auth(int fd, reader_t *r, const char *token) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AUTH %s\n", token);
    if (send_all(fd, cmd, strlen(cmd)) < 0) return -1;

    char line[MAX_LINE];
    if (read_line(r, line, sizeof(line)) < 0) return -1;
    printf("%s\n", line);
    return 0;
}

static int do_simple(int fd, reader_t *r, const char *cmd) {
    char out[MAX_LINE];
    snprintf(out, sizeof(out), "%s\n", cmd);
    if (send_all(fd, out, strlen(out)) < 0) return -1;

    char line[MAX_LINE];
    if (read_line(r, line, sizeof(line)) < 0) return -1;
    printf("%s\n", line);
    return 0;
}

static int do_put(int fd, reader_t *r, const char *filename) {
    /* 1. Read the local file to know its size */
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("  [local file '%s' not found]\n", filename);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) { fclose(fp); printf("  [file size error]\n"); return 0; }

    /* 2. Send the command line */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "PUT %s %ld\n", filename, size);
    if (send_all(fd, cmd, strlen(cmd)) < 0) { fclose(fp); return -1; }

    /* 3. Send exactly `size` raw bytes */
    unsigned char chunk[4096];
    long remaining = size;
    while (remaining > 0) {
        size_t take = (remaining > (long)sizeof(chunk)) ?
                       sizeof(chunk) : (size_t)remaining;
        size_t got = fread(chunk, 1, take, fp);
        if (got == 0) break;
        if (send_all(fd, chunk, got) < 0) { fclose(fp); return -1; }
        remaining -= (long)got;
    }
    fclose(fp);

    /* 4. Read the Agent's confirmation */
    char line[MAX_LINE];
    if (read_line(r, line, sizeof(line)) < 0) return -1;
    printf("%s\n", line);
    return 0;
}

static int do_get(int fd, reader_t *r, const char *filename) {
    /* 1. Send the command */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "GET %s\n", filename);
    if (send_all(fd, cmd, strlen(cmd)) < 0) return -1;

    /* 2. Read first line — expect "OK FILE_SEND <name> <size> SID:6180"
     *    or "ERR 005 FILE_NOT_FOUND SID:6180" */
    char line[MAX_LINE];
    if (read_line(r, line, sizeof(line)) < 0) return -1;
    printf("%s\n", line);

    if (strncmp(line, "OK FILE_SEND", 12) != 0) {
        /* error response — nothing more to read */
        return 0;
    }

    /* Parse the size (4th token: OK FILE_SEND name size SID:...) */
    char name[256] = {0}, sid[64] = {0};
    long size = -1;
    if (sscanf(line, "OK FILE_SEND %255s %ld %63s", name, &size, sid) != 3
        || size < 0) {
        printf("  [failed to parse FILE_SEND line]\n");
        return 0;
    }

    /* 3. Ensure download directory exists */
    mkdir(DOWNLOAD_DIR, 0755);

    /* 4. Open destination file and read exactly `size` bytes */
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", DOWNLOAD_DIR, filename);
    FILE *fp = fopen(path, "wb");
    if (!fp) { printf("  [cannot write %s]\n", path); return 0; }

    unsigned char chunk[4096];
    long remaining = size;
    while (remaining > 0) {
        size_t take = (remaining > (long)sizeof(chunk)) ?
                       sizeof(chunk) : (size_t)remaining;
        if (read_exact(r, chunk, take) != 0) {
            printf("  [transfer truncated]\n");
            fclose(fp);
            return -1;
        }
        if (fwrite(chunk, 1, take, fp) != take) {
            printf("  [write error]\n");
            fclose(fp);
            return -1;
        }
        remaining -= (long)take;
    }
    fclose(fp);
    printf("  [saved %ld bytes to %s]\n", size, path);
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    const char *agent_ip = (argc > 1) ? argv[1] : DEFAULT_AGENT_IP;

    printf("RemoteOps Controller — connecting to %s:%d\n", agent_ip, AGENT_PORT);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(AGENT_PORT);
    if (inet_pton(AF_INET, agent_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad IP: %s\n", agent_ip);
        exit(1);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }
    printf("connected. Type a command, or 'help' for usage.\n\n");

    reader_t r;
    r.fd = fd; r.buf_len = 0; r.buf_pos = 0;

    /* Shared flag for the UDP listener thread. */
    static volatile int udp_running = 0;

    char inbuf[MAX_LINE];
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(inbuf, sizeof(inbuf), stdin)) {
            /* EOF (Ctrl+D) → send QUIT and exit */
            send_all(fd, "QUIT\n", 5);
            char line[MAX_LINE];
            if (read_line(&r, line, sizeof(line)) >= 0) printf("%s\n", line);
            break;
        }

        /* Strip trailing newline */
        size_t n = strlen(inbuf);
        while (n > 0 && (inbuf[n-1] == '\n' || inbuf[n-1] == '\r'))
            inbuf[--n] = '\0';

        if (n == 0) continue;

        /* ---- local commands (handled by Controller, not sent) ---- */
        if (!strcmp(inbuf, "quit") || !strcmp(inbuf, "exit")) {
            send_all(fd, "QUIT\n", 5);
            char line[MAX_LINE];
            if (read_line(&r, line, sizeof(line)) >= 0) printf("%s\n", line);
            /* stop UDP listener if running */
            if (udp_running) { udp_running = 0; usleep(300000); }
            break;
        }
        if (!strcmp(inbuf, "help")) {
            printf(
                "Commands (forwarded to Agent):\n"
                "  AUTH <token>            authenticate (required first)\n"
                "  SYSINFO                 CPU/mem/uptime\n"
                "  LISTPROC                process snapshot\n"
                "  EXEC <name>             DATE|UPTIME|DISKFREE|HOSTNAME|WHOAMI\n"
                "  PUT <localfile>         upload to Agent\n"
                "  GET <filename>          download from Agent\n"
                "  MONITOR START <port>    start UDP stream on <port>\n"
                "  MONITOR STOP            stop UDP stream\n"
                "  QUIT                    close session\n"
                "Local: quit, exit, help\n");
            continue;
        }

        /* ---- parse the first two tokens to decide how to send ---- */
        char verb[64] = {0}, arg1[1024] = {0}, arg2[64] = {0};
        sscanf(inbuf, "%63s %1023s %63s", verb, arg1, arg2);

        if (!strcmp(verb, "AUTH")) {
            do_auth(fd, &r, arg1);
        }
        else if (!strcmp(verb, "PUT")) {
            if (!arg1[0]) { printf("usage: PUT <localfile>\n"); continue; }
            do_put(fd, &r, arg1);
        }
        else if (!strcmp(verb, "GET")) {
            if (!arg1[0]) { printf("usage: GET <filename>\n"); continue; }
            do_get(fd, &r, arg1);
        }
        else if (!strcmp(verb, "MONITOR") && !strcmp(arg1, "START")) {
            int port = atoi(arg2);
            if (port <= 0 || port > 65535) {
                printf("usage: MONITOR START <udp_port>\n");
                continue;
            }
            /* Send the command first */
            do_simple(fd, &r, inbuf);

            /* If the Agent accepted, start the UDP listener */
            /* (We optimistically spawn — the receiver just sits idle
             *  if the Agent rejected the command.) */
            udp_running = 1;
            udp_args_t *ua = malloc(sizeof(udp_args_t));
            ua->udp_port  = port;
            ua->running   = &udp_running;
            pthread_t tid;
            if (pthread_create(&tid, NULL, udp_listener, ua) != 0) {
                perror("pthread_create udp");
                udp_running = 0;
                free(ua);
            } else {
                pthread_detach(tid);
            }
        }
        else if (!strcmp(verb, "MONITOR") && !strcmp(arg1, "STOP")) {
            do_simple(fd, &r, inbuf);
            if (udp_running) {
                udp_running = 0;
                usleep(300000);   /* give listener time to print 'stopped' */
            }
        }
        else {
            /* Any other command is forwarded verbatim */
            do_simple(fd, &r, inbuf);
        }
    }

    close(fd);
    printf("connection closed.\n");
    return 0;
}
