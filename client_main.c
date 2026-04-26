#include "client.h"
#include "terminal.h"
#include "auth.h"
#include "math.h"
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>

/*
Client has 2 threads running simultaneously:
1. The Keyboard Thread (main): wants to call printf every time you press a key
2. The Network Thread (listener): wants to call printf every time someone else presses a key
So we need a global screenLock so keyboard and network threads don't collide
*/

pthread_mutex_t screenLock = PTHREAD_MUTEX_INITIALIZER; // shortcut to initialize mutex without needing a separate function call

int cursorLine = 1;
int cursorIndex = 0;
int myID = -1;

Role role = VIEWER;

static int login(int networkSocket) {
    AuthRequest req;
    memset(&req, 0, sizeof(req));

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
    write(networkSocket, &req, sizeof(AuthRequest));

    // receive role
    uint8_t roleReply;
    if (read(networkSocket, &roleReply, sizeof(uint8_t)) != sizeof(uint8_t))
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

void *network_listener(void *arg) {
    int networkSocket = *(int *)arg;
    struct Packet incomingPacket;

    while (1) {
        // block until server sends an update
        int bytes_read = read(networkSocket, &incomingPacket, sizeof(struct Packet));
        if (bytes_read <= 0) { // server crashed or closed connection
            pthread_mutex_lock(&screenLock);
            printf("\r\n[Network Error] Server disconnected. Press 'q' to exit.\r\n");
            pthread_mutex_unlock(&screenLock);
            exit(0);
        }

        if (incomingPacket.action == SAVE) {
            pthread_mutex_lock(&screenLock);
            printf("\033[s"); // save cursor
            printf("\033[999;1H\r\033[K"); // jump to bottom and clear line
            if (incomingPacket.index == -1) // not an admin
                printf("[!] Save denied: only ADMIN can save.");
            else
                printf("[!] Saved by '%s'.", incomingPacket.username);
            printf("\033[u"); // restore cursor
            fflush(stdout);
            pthread_mutex_unlock(&screenLock);
            continue;
        }

        if (incomingPacket.action == JOIN) {
            pthread_mutex_lock(&screenLock);
            printf("\033[s");
            printf("\033[999;1H\r\033[K");
            printf("[+] '%s' joined the doc.", incomingPacket.username);
            printf("\033[u");
            fflush(stdout);
            pthread_mutex_unlock(&screenLock);
            continue;
        }

        if (incomingPacket.action == LEAVE) {
            pthread_mutex_lock(&screenLock);
            printf("\033[s");
            printf("\033[999;1H\r\033[K");
            printf("[-] '%s' left the doc.", incomingPacket.username);
            printf("\033[u");
            fflush(stdout);
            pthread_mutex_unlock(&screenLock);
            continue;
        }

        if (incomingPacket.action == REDRAW) {
            pthread_mutex_lock(&screenLock);
            printf("\033[%d;%dH", incomingPacket.lineNumber, incomingPacket.index + 1);
            printf("%c", incomingPacket.character);
            printf("\033[%d;%dH", cursorLine, cursorIndex + 1); // restore cursor
            fflush(stdout);
            pthread_mutex_unlock(&screenLock);
            continue;
        }

        // CRITICAL SECTION
        pthread_mutex_lock(&screenLock);

        // 1. calculate coordinates
        int row = incomingPacket.lineNumber;
        int col = incomingPacket.index + 1;

        // 2. teleport to (row, col)
        printf("\033[%d;%dH", row, col);

        // 3. print the character
        if (incomingPacket.action == INSERT) {
            if (incomingPacket.character == '\n') {
                printf("\033[K"); // erase text in-line
                printf("\033[%d;1H", row + 1); // move cursor down to the new line
                printf("\033[L"); // insert a blank row

                // if someone else types before or at same time
                if (incomingPacket.userSocket != myID) {
                    if (incomingPacket.lineNumber == cursorLine && incomingPacket.index <= cursorIndex) {
                        // this client's text moved down to the new line
                        cursorIndex -= incomingPacket.index;
                        cursorLine++;
                    }
                    else if (incomingPacket.lineNumber < cursorLine) // a new line inserted above this client's line
                        cursorLine++;
                }
            }
            else {
                printf("\033[@%c", incomingPacket.character); // ANSI insert command

                // again adjust for another client's character insertion
                if (incomingPacket.userSocket != myID && incomingPacket.lineNumber == cursorLine && incomingPacket.index <= cursorIndex)
                    cursorIndex++;
            }
            printf("\033[%d;%dH", cursorLine, cursorIndex + 1); // restore to this client's tracked cursor position
        }
        else if (incomingPacket.action  == DELETE) {
            if (incomingPacket.character == '\n') {
                // teleport to line being deleted
                printf("\033[%d;1H", incomingPacket.lineNumber);
                printf("\033[M"); // ANSI delete

                // update cursor for the client who pressed backspace
                if (incomingPacket.userSocket == myID) {
                    cursorLine = incomingPacket.lineNumber - 1;
                    cursorIndex = incomingPacket.index;
                }
                else { // again adjust for another client's line deletion
                    if (incomingPacket.lineNumber == cursorLine) {
                        cursorLine--; // this client's line is being pulled up
                        cursorIndex += incomingPacket.index;
                    }
                    else if (incomingPacket.lineNumber < cursorLine)
                        cursorLine--; // this client's line is pulled up since a line above was deleted
                }
                printf("\033[%d;%dH", cursorLine, cursorIndex + 1); // restore to tracked position
            }
            else {
                printf("\033[P"); // ANSI delete command

                // again adjust for another client's character deletion
                if (incomingPacket.userSocket != myID && incomingPacket.lineNumber == cursorLine && incomingPacket.index < cursorIndex)
                    cursorIndex = max(0, cursorIndex - 1);
                printf("\033[%d;%dH", cursorLine, cursorIndex + 1); // restore to this client's tracked position
            }
        }
        else if (incomingPacket.action == MOVE) {
            cursorLine = incomingPacket.lineNumber;
            cursorIndex = incomingPacket.index;
            printf("\033[%d;%dH", cursorLine, cursorIndex + 1);
        }

        // flush buffer
        fflush(stdout);

        pthread_mutex_unlock(&screenLock);
    }
    return NULL;
}

int main() {
    int networkSocket = connect_to_server();

    if (login(networkSocket) < 0) {
        close(networkSocket);
        return 1;
    }

    read(networkSocket, &myID, sizeof(int));
    enable_raw_mode();

    // Clear the terminal screen to give us a blank canvas
    printf("\033[2J\033[H");

    // spawn the background network listener
    pthread_t listenerThread;
    pthread_create(&listenerThread, NULL, network_listener, &networkSocket);

    char c;
    struct Packet packet;

    while (1) {
        int bytes_read = read(STDIN_FILENO, &c, 1);
        if (bytes_read <= 0)
            continue;

        // Ctrl+S = ASCII 19
        if (c == 19) {
            struct Packet savePacket;
            memset(&savePacket, 0, sizeof(savePacket));
            savePacket.action = SAVE;
            savePacket.userSocket = myID;
            savePacket.timestamp = (long)time(NULL);
            write(networkSocket, &savePacket, sizeof(struct Packet));
            continue;
        }

        // the kill switch
        if (c == 'q')
            break;
        
        if (role == VIEWER) {
            if (c == '\033') {
                char seq[2];
                if (read(STDIN_FILENO, &seq[0], 1) == 0)
                    continue;
                if (read(STDIN_FILENO, &seq[1], 1) == 0)
                    continue;
                if (seq[0] == '[') {
                    packet.action = MOVE;
                    packet.lineNumber = cursorLine;
                    packet.index = cursorIndex;
                    packet.timestamp = (long)time(NULL);
                    packet.userSocket = myID;
                    packet.character = seq[1];

                    write(networkSocket, &packet, sizeof(struct Packet));
                }
                continue;
            }
            continue;
        }
        
        // enter key parser
        if (c == '\n' || c == '\r') {
            // build the packet
            packet.action = INSERT;
            packet.userSocket = myID;
            packet.timestamp = (long)time(NULL);
            packet.lineNumber = cursorLine;
            packet.index = cursorIndex;
            packet.character = '\n';
            
            // send to the server
            write(networkSocket, &packet, sizeof(struct Packet));

            cursorLine++;
            cursorIndex = 0;
            
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
                packet.action = MOVE;
                packet.lineNumber = cursorLine;
                packet.index = cursorIndex;
                packet.timestamp = (long)time(NULL);
                packet.userSocket = myID;
                packet.character = seq[1];

                write(networkSocket, &packet, sizeof(struct Packet));
            }
            continue;
        }

        // backspace parser
        if (c == 127) {
            if (cursorIndex > 0) {
                cursorIndex--;

                // build the delete packet
                packet.action = DELETE;
                packet.userSocket = myID;
                packet.timestamp = (long)time(NULL);
                packet.lineNumber = cursorLine;
                packet.index = cursorIndex;
                packet.character = '\0'; // don't care for delete

                write(networkSocket, &packet, sizeof(struct Packet));

                // visually erase it locally
                pthread_mutex_lock(&screenLock);
                printf("\033[%d;%dH", cursorLine, cursorIndex + 1);
                printf("\033[P"); // ANSI delete command
                fflush(stdout);
                pthread_mutex_unlock(&screenLock);
            }
            else if (cursorLine > 1) {
                packet.action = DELETE;
                packet.userSocket = myID;
                packet.timestamp = (long)time(NULL);
                packet.lineNumber = cursorLine;
                packet.index = -1; // code to merge with previous line
                packet.character = '\0';

                write(networkSocket, &packet, sizeof(struct Packet));

                cursorLine--;
                pthread_mutex_lock(&screenLock);
                printf("\033[%d;%dH", cursorLine, cursorIndex + 1);
                fflush(stdout);
                pthread_mutex_unlock(&screenLock);
            }
            continue;
        }

        // build the packet
        packet.action = INSERT;
        packet.userSocket = myID;
        packet.timestamp = (long)time(NULL);
        packet.lineNumber = cursorLine;
        packet.index = cursorIndex;
        packet.character = c;

        // blast it to the server
        write(networkSocket, &packet, sizeof(struct Packet));

        // CRITICAL SECTION: DRAWING TO THE SCREEN
        pthread_mutex_lock(&screenLock); // lock the screen so the network thread doesn't interrupt us

        printf("\033[%d;%dH", cursorLine, cursorIndex + 1);
        printf("\033[@%c", c);
        fflush(stdout);

        pthread_mutex_unlock(&screenLock);

        cursorIndex++;
    }
    close(networkSocket);
    return 0;
}