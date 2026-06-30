#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "network.h"
#include "protocol.h"

ssize_t read_exact(int fd, void* buf, size_t n) {
    size_t total = 0;
    char* ptr = (char *)buf;
    while (total < n) {
        ssize_t r = read(fd, ptr + total, n - total);
        if (r == 0) // clean close
            return 0;
        else if (r < 0) // error
            return -1;
        total += (size_t)r;
    }
    return (ssize_t)total;
}

ssize_t write_exact(int fd, const void* buf, size_t n) {
    size_t total = 0;
    const char* ptr = (const char *)buf;
    while (total < n) {
        ssize_t w = write(fd, ptr + total, n - total);
        if (w < 0) // error
            return -1;
        total += (size_t)w;
    }
    return (ssize_t)total;
}

int send_packet(int fd, const struct Packet* pkt) {
    uint32_t len = htonl((uint32_t)sizeof(struct Packet));
    if (write_exact(fd, &len, sizeof(len)) < 0)
        return -1;
    if (write_exact(fd, pkt, sizeof(struct Packet)) < 0)
        return -1;
    return 0;
}

int recv_packet(int fd, struct Packet* pkt, int* disconnected) {
    *disconnected = 0;
    uint32_t len_net;
    ssize_t r = read_exact(fd, &len_net, sizeof(len_net));
    if (r == 0) {
        *disconnected = 1;
        return -1;
    }
    if (r < 0) // error
        return -1;
    
    uint32_t len = ntohl(len_net);
    if (len != sizeof(struct Packet)) // framing mismatch
        return -1;
    
    r = read_exact(fd, pkt, sizeof(struct Packet));
    if (r == 0) {
        *disconnected = 1;
        return -1;
    }
    if (r < 0) // error
        return -1;
    return 0;
}