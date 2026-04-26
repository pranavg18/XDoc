#include "server.h"

int initialize_server() {
    int serverFd;
    struct sockaddr_in serverAddr;
    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd == -1) {
        perror("Socket creation failed");
        exit(1);
    }

    // tells OS to reuse port after a crash or Ctrl+C
    int opt = 1;
    if (setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        exit(1);
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverFd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed");
        exit(1);
    }

    if (listen(serverFd, backlog) < 0) {
        perror("Listen failed");
        exit(1);
    }

    printf("Server is now passively listening on port %d with a backlog of %d\n", PORT, backlog);

    return serverFd;
}