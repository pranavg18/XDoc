#ifndef AUTH_H
#define AUTH_H

#include <stdio.h>
#include <string.h>

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
    char username[32];
    char password[32];
} AuthRequest;

// returns the role for the given credentials
Role authenticate(const char *username, const char *password);

#endif
