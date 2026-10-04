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

int main(void) {
    /* 1. Create the TCP listening socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    /* Allow quick restart without "address already in use" */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 2. Bind to port 9410 on all interfaces */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(AGENT_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    printf("[agent] listening on port %d\n", AGENT_PORT);

    /* 3. Listen */
    if (listen(listen_fd, BACKLOG) < 0) { perror("listen"); exit(1); }

    /* 4. Accept one client, greet, close. (Threading comes later.) */
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) { perror("accept"); exit(1); }

    printf("[agent] client connected from %s\n",
           inet_ntoa(client_addr.sin_addr));

    /* Send a greeting line so the client sees something */
    char greeting[128];
    snprintf(greeting, sizeof(greeting),
             "OK REMOTEOPS AGENT READY SID:%s\n", SID_TAG);
    send(client_fd, greeting, strlen(greeting), 0);

    /* 5. Read one line from the client and print it */
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) printf("[agent] received: %s", buf);

    /* 6. Close */
    close(client_fd);
    close(listen_fd);
    printf("[agent] shutting down\n");
    return 0;
}
