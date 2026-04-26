#ifndef CLIENT_H
#define CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include "protocol.h"

#define PORT 8080
#define IP "127.0.0.1" // localhost for testing

int connect_to_server();

#endif