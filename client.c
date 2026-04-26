#include "client.h"

int connect_to_server() {
    int client_fd;
    struct sockaddr_in server_addr;
    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        perror("Socket creation error");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address or Address not supported");
        exit(1);
    }

    if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Server connection failed");
        exit(1);
    }

    printf("Connection connected to the server at %s: %d\n", IP, PORT);

    return client_fd;
}