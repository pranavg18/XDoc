#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <time.h>

typedef struct {
    uint32_t counter; // Lamport clock value at time of creation
    uint16_t client_id; // which client created this character
} ID;

// returns 1 if a > b, -1 if a < b, 0 if equal
static inline int id_cmp(ID a, ID b) {
    if (a.counter != b.counter) return (a.counter > b.counter) ? 1 : -1;
    if (a.client_id != b.client_id) return (a.client_id > b.client_id) ? 1 : -1;
    return 0;
}

static inline int id_eq(ID a, ID b) {
    return a.counter == b.counter && a.client_id == b.client_id;
}

// sentinel ID (dummy starting node)
static inline ID id_zero(void) {
    ID z = {0, 0};
    return z;
}

typedef enum {
    INSERT, // insert a character
    DELETE, // delete (tombstone) a character
    MOVE, // cursor movement (arrow keys), echoed back by server
    SAVE, // admin can save
    JOIN, // notification: new user joined
    LEAVE // notification: existing user disconnected
} ActionType;

struct Packet {
    ActionType action; // whether it was a type or a backspace
    int userSocket; // sender's socket fd
    long timestamp;
    ID id; // character's unique ID
    ID leftID; // anchor ID
    char character; // the actual character
    char username[32]; // to track who is asking to save
} __attribute__((packed)); // prevents compiler memory padding

// TCP framing header to allow the receiver to know exactly how many bytes to read for one packet
typedef struct {
    uint32_t length; // payload length in bytes i.e. sizeof(struct Packet)
} FrameHeader;

#endif // PROTOCOL_H