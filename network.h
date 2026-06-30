#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>
#include <sys/types.h>

struct Packet; // forward declaration

// read exactly n bytes from fd into buf
ssize_t read_exact(int fd, void* buf, size_t n); // returns n on success, 0 on clean close, -1 on error

// write exactly n bytes from buf to fd
ssize_t write_exact(int fd, const void* buf, size_t n); // returns n on success, -1 on error

// send one struct Packet with a 4-byte length prefix
int send_packet(int fd, const struct Packet* pkt); // returns 0 on success, -1 on error

// receive one struct Packet (reads the 4-byte length header first)
int recv_packet(int fd, struct Packet* pkt, int* disconnected); // returns 0 on success, 0-with-disconnect (sets *disconnected to 1) on clean close, -1 on error

#endif // NETWORK_H