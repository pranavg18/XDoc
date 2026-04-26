#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <time.h>

typedef enum {
    INSERT,
    DELETE,
    MOVE, // to figure out the length of the previous/next line when moving cursor up or down
    REDRAW, // fix cursor issue in other clients
    SAVE, // admin can save
    JOIN, // new user joined
    LEAVE // existing user disconnected
} ActionType;

struct Packet {
    ActionType action; // whether it was a type or a backspace
    int userSocket; // the actual user
    long timestamp;
    int lineNumber;
    int index;
    char character;
    char username[32]; // to track who is asking to save
} __attribute__((packed)); // prevents compiler memory padding

#endif