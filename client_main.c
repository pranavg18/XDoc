#include "client.h"
#include "terminal.h"
#include "auth.h"
#include "crdt.h"
#include "network.h"
#include "math.h"
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>

/*
Client keeps a full local CRDT replica. Every incoming INSERT/DELETE from the server is applied to this
replica so the client can compute accurate cursor positions even during concurrent edits.
*/

pthread_mutex_t screenLock = PTHREAD_MUTEX_INITIALIZER; // shortcut to initialize mutex without needing a separate function call

static CRDTDoc* doc;
static uint32_t g_lamport = 0; // local Lamport clock
static pthread_mutex_t lamportLock = PTHREAD_MUTEX_INITIALIZER;

/*
Cursor is anchored to the ID of the character immediately to the left.
{0, 0} means the cursor is at the very start of the document
*/
static ID cursor_anchor = {0, 0};
static int myID = -1;
static Role role = VIEWER;

// Lamport helpers
static uint32_t local_tick() {
    pthread_mutex_lock(&lamportLock);
    uint32_t c = ++g_lamport;
    pthread_mutex_unlock(&lamportLock);
    return c;
}

static void update_lamport(uint32_t remote) {
    pthread_mutex_lock(&lamportLock);
    if (remote > g_lamport)
        g_lamport = remote;
    g_lamport++;
    pthread_mutex_unlock(&lamportLock);
}

// maps a CRDT anchor to an ANSI screen coordinate
static void set_cursor_from_anchor(CRDTDoc* doc, ID anchor) {
    int curLine, curCol;
    crdt_pos_of(doc, anchor, &curLine, &curCol);

    if (id_eq(anchor, id_zero()))
        curLine = 1, curCol = 1;
    else {
        pthread_mutex_lock(&doc->lock);
        CharNode* anchorNode = crdt_find(doc, anchor);
        int isNewLine = anchorNode && (anchorNode->value == '\n');
        int isDeleted = anchorNode && anchorNode->deleted;
        pthread_mutex_unlock(&doc->lock);

        if (!isDeleted) {
            if (isNewLine)
                curLine++, curCol = 1;
            else
                curCol += 2; // +1 for 0-index -> 1-index ANSI, +1 for "after" anchor
        }
        else
            curCol++; // just convert to 1-indexed since cursor is already past the deleted char
    }
    printf("\033[%d;%dH", curLine, curCol);
    fflush(stdout);
}

// Full-screen redraw
static void redraw_screen() {
    // render the CRDT to a text buffer
    char buf[65536];
    crdt_render(doc, buf, sizeof(buf));

    printf("\033[2J\033[H"); // clear screen and home
    printf("%s", buf);

    // restore cursor using the helper
    set_cursor_from_anchor(doc, cursor_anchor);

    // DELETED
    // // compute the cursor's screen position from its anchor ID
    // int curLine, curCol;
    // crdt_pos_of(doc, cursor_anchor, &curLine, &curCol);

    // // compute effective cursor position based on anchor type
    // if (id_eq(cursor_anchor, id_zero())) {
    //     curLine = 1;
    //     curCol = 1;
    // }
    // else {
    //     // check if anchor is a newline
    //     pthread_mutex_lock(&doc->lock);
    //     CharNode* anchorNode = crdt_find(doc, cursor_anchor);
    //     int isNewLine = anchorNode && (anchorNode->value == '\n');
    //     pthread_mutex_unlock(&doc->lock);

    //     if (isNewLine) {
    //         curLine++; // cursor is on the next line
    //         curCol = 1; // ANSI 1-indexed
    //     }
    //     else
    //         curCol += 2; // +1 for 0-1 indexed, +1 for "after anchor"
    // }
    // printf("\033[2J\033[H"); // clear screen and home
    // printf("%s", buf);

    // // restore cursor
    // printf("\033[%d;%dH", curLine, curCol);
    // fflush(stdout);
}

// Login
static int login(int fd) {
    AuthRequest req;
    memset(&req, 0, sizeof(req));

    printf("Do you want to Login or Register [L/R]? ");
    fflush(stdout);
    char choice[10];
    if (!fgets(choice, sizeof(choice), stdin))
        return -1;
    
    if (toupper(choice[0]) == 'R') {
        req.isRegister = 1;
        printf("NEW ACCOUNT REGISTRATION\n");
    }
    else {
        req.isRegister = 0;
        printf("SYSTEM LOGIN\n");
    }

    // receive username
    printf("Username: ");
    fflush(stdout);
    if (!fgets(req.username, sizeof(req.username), stdin))
        return -1;
    req.username[strcspn(req.username, "\n")] = '\0';

    // receive password
    printf("Password: ");
    fflush(stdout);
    // hide password input
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= ~ECHO; // turn off echo bit
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    if (!fgets(req.password, sizeof(req.password), stdin))
        return -1;
    req.password[strcspn(req.password, "\n")] = '\0';
    t.c_lflag |= ECHO; // turn on echo bit
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    printf("\n");

    // send credentials
    write_exact(fd, &req, sizeof(AuthRequest));

    // receive role
    uint8_t roleReply;
    if (read_exact(fd, &roleReply, sizeof(uint8_t)) != sizeof(uint8_t))
        return -1;
    role = (Role)roleReply;

    if (role == DENIED) {
        printf("[Auth] Access denied. Wrong username or password.\n");
        return -1;
    }

    const char *roleNames[] = {"ADMIN", "EDITOR", "VIEWER"};
    printf("[Auth] Welcome! You are logged in as: %s\n", roleNames[role]);

    if (role == VIEWER)
        printf("[Auth] You are in READ-ONLY mode.\n");

    return 0;
}

// network listener thread
void *network_listener(void *arg) {
    int fd = *(int *)arg;
    struct Packet incomingPacket;

    while (1) {
        int dc = 0;
        if (recv_packet(fd, &incomingPacket, &dc) < 0) {
            pthread_mutex_lock(&screenLock);
            printf("\r\n[Network Error] Disconnected. Press Ctrl+Q to exit.\r\n");
            pthread_mutex_unlock(&screenLock);
            exit(0);
        }
        update_lamport(incomingPacket.id.counter);
        pthread_mutex_lock(&screenLock);

        if (incomingPacket.action == LEAVE) {
            printf("\033[s\033[999;1H\r\033[K[-] '%s' left.\033[u", incomingPacket.username);
            fflush(stdout);
            pthread_mutex_unlock(&screenLock);
            continue;
        }
        if (incomingPacket.action == SAVE) {
            printf("\033[s\033[999;1H\r\033[K");
            if (incomingPacket.id.counter == UINT32_MAX)
                printf("[!] SAVE denied: Only ADMIN can save.");
            else
                printf("[!] Saved by '%s'.", incomingPacket.username);
            printf("\033[u");
            fflush(stdout);
            pthread_mutex_unlock(&screenLock);
            continue;
        }

        // INSERT or DELETE from another client: apply to local CRDT replica
        if (incomingPacket.action == INSERT) {
            crdt_insert(doc, incomingPacket.leftID, incomingPacket.id, incomingPacket.character);
            redraw_screen();
        }
        else if (incomingPacket.action == DELETE) {
            crdt_delete(doc, incomingPacket.id);
            redraw_screen();
        }
        else if (incomingPacket.action == MOVE) {
            // server echoes back our MOVE with the resolved anchor ID
            cursor_anchor = incomingPacket.id;
            set_cursor_from_anchor(doc, cursor_anchor);
            // DELETE

            // int cl, cc;
            // crdt_pos_of(doc, cursor_anchor, &cl, &cc);
            // printf("\033[%d;%dH", cl, cc + 1);
            // fflush(stdout);
        }
        
        pthread_mutex_unlock(&screenLock);
    }
    return NULL;
}

int main() {
    doc = crdt_new();

    int fd = connect_to_server();

    if (login(fd) < 0) {
        close(fd);
        return 1;
    }

    read_exact(fd, &myID, sizeof(int));
    // receive the full document snapshot from the server
    {
        // the server sends INSERT packets and then JOIN for the new user, and we read packets until we receive a JOIN packet addressed to us
        while (1) {
            struct Packet pkt;
            int dc = 0;
            if (recv_packet(fd, &pkt, &dc) < 0)
                break;
            if (pkt.action == INSERT)
                crdt_insert(doc, pkt.leftID, pkt.id, pkt.character);
            else // received something other than INSERT
                break;
        }
    }

    enable_raw_mode();

    // Clear the terminal screen to give us a blank canvas
    printf("\033[2J\033[H");
    redraw_screen();

    // spawn the background network listener
    pthread_t listenerThread;
    pthread_create(&listenerThread, NULL, network_listener, &fd);

    char c;

    while (1) {
        int bytes_read = read(STDIN_FILENO, &c, 1);
        if (bytes_read <= 0)
            continue;

        // quit
        if (c == 17) // Ctrl+Q = ASCII 17
            break;

        // save
        if (c == 19) { // Ctrl+S = ASCII 19
            struct Packet savePacket;
            memset(&savePacket, 0, sizeof(savePacket));
            savePacket.action = SAVE;
            savePacket.userSocket = myID;
            savePacket.timestamp = (long)time(NULL);
            send_packet(fd, &savePacket);
            continue;
        }

        // arrow key parser
        if (c == '\033') {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 0)
                continue;
            if (read(STDIN_FILENO, &seq[1], 1) == 0)
                continue;
            if (seq[0] == '[') {
                struct Packet packet;
                memset(&packet, 0, sizeof(packet));
                packet.action = MOVE;
                packet.userSocket = myID;
                packet.timestamp = (long)time(NULL);
                packet.id = cursor_anchor; // current anchor
                packet.character = seq[1];
                send_packet(fd, &packet);
            }
            continue;
        }

        if (role == VIEWER)
            continue;
        
        // enter key parser
        if (c == '\n' || c == '\r') {
            ID newID = {local_tick(), (uint16_t)myID};
            crdt_insert(doc, cursor_anchor, newID, '\n');

            struct Packet packet;
            memset(&packet, 0, sizeof(packet));
            // build the packet
            packet.action = INSERT;
            packet.userSocket = myID;
            packet.timestamp = (long)time(NULL);
            packet.id = newID;
            packet.leftID = cursor_anchor;
            packet.character = '\n';
            
            // send to the server
            send_packet(fd, &packet);

            cursor_anchor = newID;

            pthread_mutex_lock(&screenLock);
            redraw_screen();
            pthread_mutex_unlock(&screenLock);
            continue;
        }

        // backspace parser
        if (c == 127) {
            if (!id_eq(cursor_anchor, id_zero())) {
                // delete the character at cursor_anchor
                ID toDelete = cursor_anchor;

                // move anchor to the node before cursor_anchor
                pthread_mutex_lock(&doc->lock);
                CharNode* curNode = crdt_find(doc, cursor_anchor);
                CharNode* prev = curNode ? curNode->prev : NULL;
                // skip over tombstoned nodes to find the nearest live predecessor
                while (prev && prev->deleted)
                    prev = prev->prev;
                ID newAnchor = prev ? prev->id : id_zero();
                pthread_mutex_unlock(&doc->lock);

                crdt_delete(doc, toDelete);
                cursor_anchor = newAnchor;

                struct Packet packet;
                memset(&packet, 0, sizeof(packet));
                packet.action = DELETE;
                packet.userSocket = myID;
                packet.timestamp = (long)time(NULL);
                packet.id = toDelete;
                send_packet(fd, &packet);

                pthread_mutex_lock(&screenLock);
                redraw_screen();
                pthread_mutex_unlock(&screenLock);
            }
            continue;
        }

        // printable character
        {
            ID newID = {local_tick(), (uint16_t)myID};
            ID oldAnchor = cursor_anchor; // save the old anchor

            crdt_insert(doc, cursor_anchor, newID, c);
            cursor_anchor = newID;

            struct Packet packet;
            memset(&packet, 0, sizeof(packet));

            // build the packet
            packet.action = INSERT;
            packet.userSocket = myID;
            packet.timestamp = (long)time(NULL);
            packet.id = newID;
            packet.leftID = oldAnchor;
            packet.character = c;

            // blast it to the server
            send_packet(fd, &packet);

            // CRITICAL SECTION: DRAWING TO THE SCREEN
            pthread_mutex_lock(&screenLock); // lock the screen so the network thread doesn't interrupt us
            redraw_screen();
            pthread_mutex_unlock(&screenLock);
        }
    }
    close(fd);
    crdt_free(doc);
    return 0;
}