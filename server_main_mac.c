#include "server.h"
#include "document.h"
#include "persistence.h"
#include "auth.h"
#include "math.h"
#include <signal.h>
#include <semaphore.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <stdarg.h>

#define MAX_CLIENTS 10 // semaphore cap

static volatile int hasSaved = 0; // set to 1 the first time admin saves
static volatile long lastSavedTime;
static pthread_mutex_t saveLock = PTHREAD_MUTEX_INITIALIZER; // guards hasSaved and lastSavedTime

Role clientRoles[100];
int activeClients[100];
char clientUsernames[100][32];
int clientCount = 0;
pthread_mutex_t clientsLock = PTHREAD_MUTEX_INITIALIZER;

sem_t *clientSlots; // counting semaphore (pointer coz fuck macOS)

// FIFO logger
#define FIFO_PATH "/tmp/editor_log"
static int logFifoFd = -1;

static void log_event(const char *fmt, ...) {
    // send to the logger process via FIFO if it is open
    if (logFifoFd != -1) {
        char buf[256];
        va_list args2;
        va_start(args2, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args2);
        va_end(args2);

        write(logFifoFd, buf, strlen(buf));
    }
    else { // print to server's own stdout if FIFO is closed
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
}

// per-client thread
typedef struct {
    int socket;
    Role role;
    char username[32];
} ClientInfo;

// SIGINT
volatile sig_atomic_t should_shutdown = 0; // read and write atomically from memory

void sigint_handler(int sig) {
    (void)sig; // remove unused-parameter warning
    should_shutdown = 1; // set the flag
}

void register_sigint_handler() {
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask); // don't block any other signals while handling
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction failed");
        exit(1);
    }
    printf("[Server] SIGINT handler registered. Press Ctrl+C to save & shut down.\n");
}

// broadcast to all other clients
static void broadcast(const struct Packet *packet, int excludeSocket) {
    pthread_mutex_lock(&clientsLock);
    for (int i = 0; i < clientCount; i++)
        if (activeClients[i]!= excludeSocket)
            write(activeClients[i], packet, sizeof(struct Packet));
    pthread_mutex_unlock(&clientsLock);
}

// broadcast to all clients
static void broadcast_all(const struct Packet *packet) {
    pthread_mutex_lock(&clientsLock);
    for (int i = 0; i < clientCount; i++)
        write(activeClients[i], packet, sizeof(struct Packet));
    pthread_mutex_unlock(&clientsLock);
}

// remove disconnected clients
static void remove_client(int clientSocket) {
    pthread_mutex_lock(&clientsLock);

    char leavingUsername[32];
    leavingUsername[0] = '\0';

    for (int i = 0; i < clientCount; i++)
        if (activeClients[i] == clientSocket) {
            strncpy(leavingUsername, clientUsernames[i], 32);
            
            // left-shift everyone after it
            for (int j = i; j < clientCount-1; j++) {
                activeClients[j] = activeClients[j+1];
                clientRoles[j] = clientRoles[j+1];
                memcpy(clientUsernames[j], clientUsernames[j+1], 32);
            }
            clientCount--;
            break;
        }

    pthread_mutex_unlock(&clientsLock);
    
    // notify remaining clients that someone left
    if (leavingUsername[0] != '\0') {
        struct Packet leavePacket;
        memset(&leavePacket, 0, sizeof(leavePacket));
        leavePacket.action = LEAVE;
        leavePacket.userSocket = clientSocket;
        leavePacket.timestamp = (long)time(NULL);
        snprintf(leavePacket.username, 32, "%s", leavingUsername);
        broadcast(&leavePacket, clientSocket);
    }

    sem_post(clientSlots); // release the semaphore slot
}

static void sync_document_to_client(int clientSocket) {
    LineNode *newUserNode = documentHead;
    while (newUserNode) {
        pthread_mutex_lock(&newUserNode->lock);
        int len = strlen(newUserNode->text);

        for (int i = 0; i < len; i++) {
            struct Packet newPacket;
            newPacket.action = INSERT;
            newPacket.userSocket = clientSocket;
            newPacket.timestamp = (long)time(NULL);
            newPacket.lineNumber = newUserNode->lineNumber;
            newPacket.index = i;
            newPacket.character = newUserNode->text[i];

            write(clientSocket, &newPacket, sizeof(struct Packet));
        }

        if (newUserNode->next) {
            struct Packet newLinePacket;
            newLinePacket.action = INSERT;
            newLinePacket.userSocket = clientSocket;
            newLinePacket.timestamp = (long)time(NULL);
            newLinePacket.lineNumber = newUserNode->lineNumber;
            newLinePacket.index = len;
            newLinePacket.character = '\n';

            write(clientSocket, &newLinePacket, sizeof(struct Packet));
        }

        pthread_mutex_unlock(&newUserNode->lock);

        newUserNode = newUserNode->next;
    }
}

void *client_handler(void *arg) {
    ClientInfo *info = (ClientInfo *)arg;
    int clientSocket = info->socket;
    Role clientRole = info->role;
    char username[32];
    strncpy(username, info->username, 32);
    free(info);
    char buffer[1024];
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        int bytes_read = read(clientSocket, buffer, sizeof(buffer)-1);
        if (bytes_read <= 0) {
            log_event("Client on socket %d disconnected.\n", clientSocket);
            break;
        }

        // parse the incoming key stroke
        int numPackets = bytes_read/sizeof(struct Packet); // no. of structs received
        struct Packet *incomingPackets = (struct Packet *)buffer; // create array of packets
        for (int i = 0; i < numPackets; i++) { // loop through every packet in case copy-paste was done
            struct Packet currentPacket = incomingPackets[i];
            printf("Action %d: User %d typed '%c' at line %d, index %d\n",
                    currentPacket.action,
                    currentPacket.userSocket,
                    currentPacket.character,
                    currentPacket.lineNumber,
                    currentPacket.index);
            
            // find the correct line
            LineNode *currentLine = documentHead;
            LineNode *prevLine = NULL;
            while (currentLine && currentLine->lineNumber != currentPacket.lineNumber) {
                prevLine = currentLine;
                currentLine = currentLine->next;
            }
            if (currentLine == NULL) { // dynamically build the document
                while (prevLine->lineNumber < currentPacket.lineNumber) {
                    // lock the tail of document before modifying its pointers
                    pthread_mutex_lock(&prevLine->lock);

                    // check if another thread built this line
                    if (prevLine->next) {
                        pthread_mutex_unlock(&prevLine->lock);
                        prevLine = prevLine->next;
                        continue;
                    }

                    LineNode *newLine = create_node(prevLine->lineNumber+1, "");
                    newLine->prev = prevLine;
                    prevLine->next = newLine;
                    pthread_mutex_unlock(&prevLine->lock);
                    prevLine = prevLine->next;
                }
                currentLine = prevLine;
            }
            
            // lock the line
            pthread_mutex_lock(&currentLine->lock);

            if (currentPacket.action == MOVE) { // if moving cursor up or down
                int newLineNum = currentPacket.lineNumber;
                int newIndex = currentPacket.index;

                if (currentPacket.character == 'A') { // UP
                    if (currentLine->prev) {
                        LineNode *target = currentLine->prev;
                        newLineNum--;

                        pthread_mutex_lock(&target->lock);
                        newIndex = min(newIndex, (int)strlen(target->text));
                        pthread_mutex_unlock(&target->lock);
                    }
                }
                else if (currentPacket.character == 'B') { // DOWN
                    if (currentLine->next) {
                        LineNode *target = currentLine->next;
                        newLineNum++;

                        pthread_mutex_lock(&target->lock);
                        newIndex = min(newIndex, (int)strlen(target->text));
                        pthread_mutex_unlock(&target->lock);
                    }
                    else
                        newIndex = (int)strlen(currentLine->text);
                }
                else if (currentPacket.character == 'C') { // RIGHT
                    int curLen = strlen(currentLine->text);
                    if (newIndex < curLen)
                        newIndex++;
                    else if (currentLine->next) {
                        newLineNum++;
                        newIndex = 0;
                    }
                }
                else if (currentPacket.character == 'D') { // LEFT
                    if (newIndex > 0)
                        newIndex--;
                    else if (currentLine->prev) {
                        newLineNum--;
                        LineNode *prevLine = currentLine->prev;
                        pthread_mutex_lock(&prevLine->lock);
                        newIndex = strlen(prevLine->text);
                        pthread_mutex_unlock(&prevLine->lock);
                    }
                }

                pthread_mutex_unlock(&currentLine->lock);

                // send it back to the client who moved the cursor
                struct Packet moveCursor;
                moveCursor.action = MOVE;
                moveCursor.lineNumber = newLineNum;
                moveCursor.index = newIndex;
                moveCursor.timestamp = (long)time(NULL);
                moveCursor.userSocket = currentPacket.userSocket;
                moveCursor.character = currentPacket.character;

                write(clientSocket, &moveCursor, sizeof(struct Packet));
                continue;
            }

            // drop the packets from viewers
            if (clientRole == VIEWER)
                continue;
            
            // update the text
            if (currentPacket.action == SAVE) { // admin only
                struct Packet reply;
                memset(&reply, 0, sizeof(reply));
                reply.action = SAVE;
                reply.userSocket = clientSocket;
                reply.timestamp = (long)time(NULL);

                pthread_mutex_unlock(&currentLine->lock); // unlock before save_document else it freezes

                if (clientRole != ADMIN) {
                    reply.index = -1; // DENIED
                    write(clientSocket, &reply, sizeof(struct Packet));
                    log_event("[Save] DENIED for socket %d ('%s') - not an admin.\n", clientSocket, username);
                }
                else {
                    save_document(DOC_FILE);
                    pthread_mutex_lock(&saveLock);
                    lastSavedTime = (long)time(NULL);
                    hasSaved = 1;
                    pthread_mutex_unlock(&saveLock);
                    reply.index = 0; // SUCCESS
                    snprintf(reply.username, 32, "%s", username);
                    broadcast_all(&reply); // everyone gets the confirmation
                    log_event("[Save] Document saved by admin '%s'.\n", username);
                }
                continue;
            }
            else if (currentPacket.action == INSERT) { // if inserting a new character
                if (currentPacket.character == '\n') {
                    // first split
                    split_line(currentLine, currentPacket.index);

                    // broadcast it to all clients
                    broadcast_all(&currentPacket);

                    // redraw the characters that moved to the new line
                    LineNode *newLine = currentLine->next;
                    int len = strlen(newLine->text);

                    for (int j = 0; j < len; j++) {
                        struct Packet redrawPacket;
                        memset(&redrawPacket, 0, sizeof(redrawPacket));
                        redrawPacket.action = REDRAW; // clients should not adjust cursor for redraws
                        redrawPacket.userSocket = currentPacket.userSocket;
                        redrawPacket.lineNumber = newLine->lineNumber;
                        redrawPacket.index = j;
                        redrawPacket.character = newLine->text[j];

                        // broadcast each character
                        broadcast_all(&redrawPacket);
                    }
                    pthread_mutex_unlock(&currentLine->lock);
                    continue;
                }
                else {
                    int len = strlen(currentLine->text);
                    if (len >= 255) {
                        pthread_mutex_unlock(&currentLine->lock);
                        continue;
                    }
                    for (int i = len; i >= currentPacket.index; i--) currentLine->text[i+1] = currentLine->text[i];
                    currentLine->text[currentPacket.index] = currentPacket.character;
                }
            }
            else if (currentPacket.action == DELETE) { // if deleting
                if (currentPacket.index == -1) { // backspace at start of line
                    LineNode *prevLine = currentLine->prev;
                    if (!prevLine) {
                        pthread_mutex_unlock(&currentLine->lock);
                        continue;
                    }

                    pthread_mutex_lock(&prevLine->lock);
                    int prevLen = strlen(prevLine->text);

                    merge_lines(currentLine);

                    // broadcast the merge command
                    struct Packet merge;
                    merge.action = DELETE;
                    merge.userSocket = currentPacket.userSocket;
                    merge.lineNumber = currentPacket.lineNumber;
                    merge.index = prevLen;
                    merge.character = '\n'; // code to pull lines up if newline in delete

                    broadcast_all(&merge);

                    // redraw merged text onto previous line
                    int newTotalLen = strlen(prevLine->text);
                    for (int j = prevLen; j < newTotalLen; j++) {
                        struct Packet redrawPacket;
                        redrawPacket.action = REDRAW; // clients should not adjust cursor for redraws
                        redrawPacket.userSocket = currentPacket.userSocket;
                        redrawPacket.lineNumber = prevLine->lineNumber;
                        redrawPacket.index = j;
                        redrawPacket.character = prevLine->text[j];

                        broadcast_all(&redrawPacket);
                    }
                    pthread_mutex_unlock(&prevLine->lock);
                    pthread_mutex_unlock(&currentLine->lock);
                    free(currentLine);
                    continue;
                }
                else {
                    int len = strlen(currentLine->text);
                    if (len > 0 && currentPacket.index < len) {
                        for (int i = currentPacket.index; i <= len; i++) currentLine->text[i] = currentLine->text[i+1];
                    }
                }
            }

            // Unlock the line
            pthread_mutex_unlock(&currentLine->lock);

            // Send the updated line to all other clients
            broadcast(&currentPacket, clientSocket);
        }
    }
    log_event("Client '%s' (socket %d) disconnected.\n", username, clientSocket);
    remove_client(clientSocket); // release semaphore
    close(clientSocket);
    pthread_exit(NULL);
}

int main() {
    load_document(DOC_FILE); // load doc from disk
    register_sigint_handler(); // SIGINT setup

    // open write-end of the FIFO (blocks until logger opens the read-end)
    log_event("[Server] Opening FIFO '%s'... (start ./logger first)\n", FIFO_PATH);
    logFifoFd = open(FIFO_PATH, O_WRONLY);
    if (logFifoFd == -1)
        printf("[Server] WARNING: Could not open FIFO. Logging to stdout only.\n");
    else
        log_event("[Server] FIFO connected to logger.\n");

    sem_unlink("/clientSlots");  // clean up any leftover from a previous crash
    clientSlots = sem_open("/clientSlots", O_CREAT, 0644, MAX_CLIENTS);
    if (clientSlots == SEM_FAILED) {
        perror("sem_open failed");
        exit(1);
    }
    log_event("[Server] Client semaphore initialized. Max clients: %d\n", MAX_CLIENTS);
    
    int serverSocket = initialize_server();

    log_event("[Server] Listening on port %d\n", PORT);
    
    while (!should_shutdown) {
        // semaphore wait blocks if MAX_CLIENTS already connected
        while (sem_trywait(clientSlots) == -1) {
            if (should_shutdown) goto shutdown;
            sleep(1);  // wait 1 second then try again
        }
        
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        printf("[Server] Waiting for a connection...\n");

        int clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientSocket < 0) {
            sem_post(clientSlots);
            if (should_shutdown)
                break;
            perror("Accept failed");
            continue;
        }
        printf("[Server] Client connected! Socket: %d\n", clientSocket);

        // role-based authentication handshake
        AuthRequest authReq;
        int authBytes = read(clientSocket, &authReq, sizeof(AuthRequest));
        if (authBytes != sizeof(AuthRequest)) {
            printf("[Auth] Incomplete auth from socket %d. Closing.\n", clientSocket);
            close(clientSocket);
            sem_post(clientSlots);
            continue;
        }
        Role role = authenticate(authReq.username, authReq.password);
        uint8_t roleReply = (uint8_t)role;
        write(clientSocket, &roleReply, sizeof(uint8_t));

        if (role == DENIED) {
            printf("[Auth] Login DENIED for '%s'\n", authReq.username);
            close(clientSocket);
            sem_post(clientSlots);
            continue;
        }

        const char *roleNames[] = {"ADMIN", "EDITOR", "VIEWER"};
        log_event("[Auth] '%s' logged in as %s (socket %d)\n", authReq.username, roleNames[role], clientSocket);

        // give the client its ID so it becomes self-aware
        write(clientSocket, &clientSocket, sizeof(int));
        sync_document_to_client(clientSocket);

        pthread_mutex_lock(&clientsLock);
        // add new socket to active clients list
        activeClients[clientCount] = clientSocket;
        clientRoles[clientCount] = role;
        strncpy(clientUsernames[clientCount], authReq.username, 32);
        clientCount++;

        pthread_mutex_unlock(&clientsLock);

        // notify every other client that a new user joined
        struct Packet joinPacket;
        memset(&joinPacket, 0, sizeof(joinPacket));
        joinPacket.action = JOIN;
        joinPacket.userSocket = clientSocket;
        joinPacket.timestamp = (long)time(NULL);
        snprintf(joinPacket.username, 32, "%s", authReq.username);
        broadcast(&joinPacket, clientSocket);

        ClientInfo *info = malloc(sizeof(ClientInfo));
        info->socket = clientSocket;
        info->role = role;
        strncpy(info->username, authReq.username, 32);

        pthread_t clientThread;
        if (pthread_create(&clientThread, NULL, client_handler, info) < 0) {
            perror("Could not create thread");
            free(info);
            remove_client(clientSocket);
            close(clientSocket);
        }
        else
            pthread_detach(clientThread);
    }
    shutdown:
    printf("\n[Server] Shutting down.\n");
    pthread_mutex_lock(&saveLock);
    long actualSavedTime = lastSavedTime;
    pthread_mutex_unlock(&saveLock);
    if (!hasSaved)
        printf("[WARNING] The document was never saved to disk during this session.\nAll changes are lost. Next time, have admin press Ctrl+S before quitting.\n");
    else {
        time_t raw_time = (time_t)actualSavedTime;
        struct tm *info;
        char buffer[80];

        // convert to local time structure
        info = localtime(&raw_time);

        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", info);
        printf("[Server] Document was last saved by an admin during this session at time %s.\n", buffer);
    }

    sem_close(clientSlots);
    sem_unlink("/clientSlots"); // remove it from the OS entirely
    close(serverSocket);
    printf("[Server] Goodbye.\n");
    return 0;
}