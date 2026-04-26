#include "server.h"
#include "document.h"
#include "persistence.h"
#include "auth.h"
#include "math.h"
#include <signal.h>
#include <semaphore.h>
#include <stdint.h>
#include <time.h>

#define MAX_CLIENTS 10 // semaphore cap

Role clientRoles[100];
int activeClients[100];
int clientCount = 0;
pthread_mutex_t clientsLock = PTHREAD_MUTEX_INITIALIZER;

sem_t clientSlots; // counting semaphore

// per-client thread
typedef struct {
    int socket;
    Role role;
} ClientInfo;

// ─── SIGINT / graceful shutdown ───────────────────────────────────────────────
// We set this flag inside the signal handler (async-signal-safe) and check it
// in the main accept() loop. Avoids calling non-reentrant functions from the
// handler itself.  Maps to Exercise 59b (catching SIGINT) and 62 (sigaction).
volatile sig_atomic_t should_shutdown = 0; // read and write atomically from memory

void sigint_handler(int sig) {
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
            write(activeClients[i], &packet, sizeof(struct Packet));
    pthread_mutex_unlock(&clientsLock);
}

// broadcast to all clients
static void broadcast_all(const struct Packet *packet) {
    pthread_mutex_lock(&clientsLock);
    for (int i = 0; i < clientCount; i++)
        write(activeClients[i], &packet, sizeof(struct Packet));
    pthread_mutex_unlock(&clientsLock);
}

// remove disconnected clients
static void remove_client(int clientSocket) {
    pthread_mutex_lock(&clientsLock);

    for (int i = 0; i < clientCount; i++)
        if (activeClients[i] == clientSocket) {
            // left-shift everyone after it
            for (int j = i; j < clientCount-1; j++) {
                activeClients[j] = activeClients[j+1];
                clientRoles[j] = clientRoles[j+1];
            }
            clientCount--;
            break;
        }

    pthread_mutex_unlock(&clientsLock);
    sem_post(&clientSlots); // release the semaphore slot
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
    free(info);
    char buffer[1024];
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        int bytes_read = read(clientSocket, buffer, sizeof(buffer)-1);
        if (bytes_read <= 0) {
            printf("Client on socket %d disconnected.\n", clientSocket);
            break;
        }
        // printf("Received from %d: %s\n", clientSocket, buffer);

        // drop the packets from viewers
        if (clientRole == VIEWER)
            continue;

        // 1. Parse the incoming key stroke
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
            
            // Find the correct line
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
            
            // Lock the line
            pthread_mutex_lock(&currentLine->lock);

            // Update the text
            if (currentPacket.action == INSERT) { // if inserting a new character
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
                        redrawPacket.action = INSERT;
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
                    if (len >= 255) continue;
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
                        redrawPacket.action = INSERT;
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
            else if (currentPacket.action == MOVE) { // if moving cursor up or down
                int newLineNum = currentPacket.lineNumber;
                int newIndex = currentPacket.index;

                if (currentPacket.character == 'A') { // UP
                    if (currentLine->prev) {
                        LineNode *target = currentLine->prev;
                        newLineNum = target->lineNumber;

                        pthread_mutex_lock(&target->lock);
                        int targetLen = strlen(target->text);
                        pthread_mutex_unlock(&target->lock);

                        newIndex = min(currentPacket.index, targetLen);
                    }
                }
                else if (currentPacket.character == 'B') { // DOWN
                    if (currentLine->next) {
                        LineNode *target = currentLine->next;
                        newLineNum = target->lineNumber;

                        pthread_mutex_lock(&target->lock);
                        int targetLen = strlen(target->text);
                        pthread_mutex_unlock(&target->lock);

                        newIndex = min(currentPacket.index, targetLen);
                    }
                }
                else if (currentPacket.character == 'C') { // RIGHT
                    pthread_mutex_lock(&currentLine->lock);
                    int curLen = strlen(currentLine->text);
                    pthread_mutex_unlock(&currentLine->lock);
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

                // send it back to the client who moved the cursor
                struct Packet moveCursor;
                moveCursor.action = MOVE;
                moveCursor.lineNumber = newLineNum;
                moveCursor.index = newIndex;
                moveCursor.timestamp = (long)time(NULL);
                moveCursor.userSocket = currentPacket.userSocket;
                moveCursor.character = currentPacket.character;

                write(clientSocket, &moveCursor, sizeof(struct Packet));
            }

            // Unlock the line
            pthread_mutex_unlock(&currentLine->lock);

            // Send the updated line to all other clients
            broadcast(&currentPacket, clientSocket);
        }
    }
    close(clientSocket);
    pthread_exit(NULL);
}

int main() {
    // init_document();
    load_document(DOC_FILE); // load doc from disk
    register_sigint_handler(); // SIGINT setup
    if (sem_init(&clientSlots, 0, MAX_CLIENTS) == -1) { // initialize counting semaphore to MAX_CLIENTS
        perror("sem_init failed");
        exit(1);
    }
    printf("[Server] Client semaphore initialized. Max clients: %d\n", MAX_CLIENTS);
    
    int serverSocket = initialize_server();

    // if (listen(serverSocket, backlog) < 0) {
    //     perror("Listen failed");
    //     exit(1);
    // }
    printf("[Server] Listening on port %d\n", PORT);
    
    while (!should_shutdown) {
        // semaphore wait blocks if MAX_CLIENTS already connected
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1; // 1-sec timeout so SIGINT is not ignored for too long
        if (sem_timedwait(&clientSlots, &ts) == -1) // either timeout (EAGAIN/ETIMEDOUT) or interrupted by signal
            continue; // loop back and re-check should_shutdown
        
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        printf("[Server] Waiting for a connection...\n");

        int clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientSocket < 0) {
            sem_post(&clientSlots);
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
            sem_post(&clientSlots);
            continue;
        }
        Role role = authenticate(authReq.username, authReq.password);
        uint8_t roleReply = (uint8_t)role;
        write(clientSocket, &roleReply, sizeof(uint8_t));

        if (role == DENIED) {
            printf("[Auth] Login DENIED for '%s'\n", authReq.username);
            close(clientSocket);
            sem_post(&clientSlots);
            continue;
        }

        const char *roleNames[] = {"ADMIN", "EDITOR", "VIEWER"};
        printf("[Auth] '%s' logged in as %s (socket %d)\n", authReq.username, roleNames[role], clientSocket);


        // give the client its ID so it becomes self-aware
        write(clientSocket, &clientSocket, sizeof(int));
        sync_document_to_client(clientSocket);

        pthread_mutex_lock(&clientsLock);
        // add new socket to active clients list
        activeClients[clientCount] = clientSocket;
        activeClients[clientCount] = role;
        clientCount++;

        pthread_mutex_unlock(&clientsLock);

        // pthread_mutex_lock(&clientsLock);
        
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

        // pthread_mutex_unlock(&clientsLock);

        ClientInfo *info = malloc(sizeof(ClientInfo));
        info->socket = clientSocket;
        info->role = role;

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
    printf("\n[Server] Shutting down. Saving document...\n");
    save_document(DOC_FILE);

    sem_destroy(&clientSlots);
    close(serverSocket);
    printf("[Server] Goodbye.\n");
    return 0;
}