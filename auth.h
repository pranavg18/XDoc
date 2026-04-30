#ifndef AUTH_H
#define AUTH_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// users.txt format is username:password:role
#define USERS_FILE "users.txt"

/*
Admin can read, edit and also save to disk
Editor can read and edit
Viewer can just read
*/

typedef enum {
    ADMIN = 0,
    EDITOR = 1,
    VIEWER = 2,
    DENIED = 3 // wrong password
} Role;

// auth packet sent from client to server right after connecting
typedef struct {
    uint8_t isRegister; // 0 for login, 1 for register
    char username[32];
    char password[32];
} AuthRequest;

// returns the role for the given credentials
Role authenticate(const char *username, const char *password);

// creates a new user
Role register_user(const char *username, const char *password);

#endif
