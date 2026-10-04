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
        else {
            /* Authenticated, but command not implemented yet.
             * Steps D+ will add SYSINFO / LISTPROC / EXEC / PUT / GET /
             * MONITOR START|STOP handlers here. */
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
