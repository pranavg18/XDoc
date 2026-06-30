#include "server.h"
#include "crdt.h"
#include "persistence.h"
#include "auth.h"
#include "network.h"
#include "math.h"
#include <signal.h>
#include <semaphore.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <stdarg.h>

#define MAX_CLIENTS 10 // semaphore cap

// global document state
static CRDTDoc* doc;
static uint32_t g_lamport = 0; // server-side Lamport counter (for sync packets)
static pthread_mutex_t lamport_lock = PTHREAD_MUTEX_INITIALIZER;

// save state
static volatile int hasSaved = 0; // set to 1 the first time admin saves
static volatile long lastSavedTime;
static pthread_mutex_t saveLock = PTHREAD_MUTEX_INITIALIZER; // guards hasSaved and lastSavedTime

// client registry
static Role clientRoles[100];
static int activeClients[100];
static char clientUsernames[100][32];
static int clientCount = 0;
static pthread_mutex_t clientsLock = PTHREAD_MUTEX_INITIALIZER;

static sem_t *clientSlots; // counting semaphore (pointer for heap allocation)

// per-client thread
typedef struct {
    int socket;
    Role role;
    char username[32];
} ClientInfo;

// FIFO logger
#define FIFO_PATH "/tmp/editor_log"
static int logFifoFd = -1;

static void log_event(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (logFifoFd != -1) // send to the logger process via FIFO if it is open
        write(logFifoFd, buf, strlen(buf));
    else // print to server's own stdout if FIFO is closed
        printf("%s", buf);
}

// SIGINT
volatile sig_atomic_t should_shutdown = 0; // read and write atomically from memory

static void sigint_handler(int sig) {
    (void)sig; // remove unused-parameter warning
    should_shutdown = 1; // set the flag
}

static void register_sigint_handler() {
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

// helpers
static uint32_t next_lamport(void) {
    pthread_mutex_lock(&lamport_lock);
    uint32_t c = ++g_lamport;
    pthread_mutex_unlock(&lamport_lock);
    return c;
}

// broadcast to all other clients
static void broadcast(const struct Packet *packet, int excludeSocket) {
    pthread_mutex_lock(&clientsLock);
    for (int i = 0; i < clientCount; i++)
        if (activeClients[i]!= excludeSocket)
            send_packet(activeClients[i], packet);
    pthread_mutex_unlock(&clientsLock);
}

// broadcast to all clients
static void broadcast_all(const struct Packet *packet) {
    pthread_mutex_lock(&clientsLock);
    for (int i = 0; i < clientCount; i++)
        send_packet(activeClients[i], packet);
    pthread_mutex_unlock(&clientsLock);
}

// remove disconnected clients
static void remove_client(int clientSocket) {
    pthread_mutex_lock(&clientsLock);

    char leavingUsername[32] = {0};

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
    pthread_mutex_lock(&doc->lock);
    CharNode* cur = doc->head->next;
    while (cur) {
        if (!cur->deleted) {
            struct Packet pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.action = INSERT;
            pkt.userSocket = clientSocket;
            pkt.timestamp = (long)time(NULL);
            pkt.id = cur->id;
            pkt.leftID = cur->leftID;
            pkt.character = cur->value;
            send_packet(clientSocket, &pkt);
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&doc->lock);
}

// per-client thread
void *client_handler(void *arg) {
    ClientInfo *info = (ClientInfo *)arg;
    int clientSocket = info->socket;
    Role clientRole = info->role;
    char username[32];
    strncpy(username, info->username, 32);
    free(info);

    while (1) {
        struct Packet pkt;
        int disconnected = 0;
        if (recv_packet(clientSocket, &pkt, &disconnected) < 0) {
            if (!disconnected)
                log_event("Client on socket %d disconnected.\n", clientSocket);
            break;
        }

        // MOVE: compute new position and echo back to sender
        if (pkt.action == MOVE) {
            int cur_line, cur_col;
            crdt_pos_of(doc, pkt.id, &cur_line, &cur_col);
            cur_col++; // cursor is after the anchor, not on it
            int newLine = cur_line, newCol = cur_col;

            if (pkt.character == 'A') { // UP
                if (newLine > 1) {
                    newLine--;
                    // find the lenght of the target line
                    char buf[65536];
                    crdt_render(doc, buf, sizeof(buf));
                    // walk rendered lines to find length
                    int l = 1, colMax = 0;
                    for (char *p = buf; *p; p++) {
                        if (l == newLine){
                            if (*p == '\n')
                                break;
                            colMax++;
                        }
                        if (*p == '\n')
                            l++;
                    }
                    newCol = (newCol < colMax) ? newCol : colMax;
                }
            }
            else if (pkt.character == 'B') { // DOWN
                char buf[65536];
                crdt_render(doc, buf, sizeof(buf));
                int totalLines = 1;
                for (char *p = buf; *p; p++)
                    if (*p == '\n')
                        totalLines++;
                if (newLine < totalLines) {
                    newLine++;
                    int l = 1, colMax = 0;
                    for (char *p = buf; *p; p++) {
                        if (l == newLine){
                            if (*p == '\n')
                                break;
                            colMax++;
                        }
                        if (*p == '\n')
                            l++;
                    }
                    newCol = (newCol < colMax) ? newCol : colMax;
                }
            }
            else if (pkt.character == 'C') { // RIGHT
                char buf[65536];
                crdt_render(doc, buf, sizeof(buf));
                int l = 1, c = 0;
                for (char *p = buf; *p; p++) {
                    if (l == cur_line && c == cur_col) {
                        if (*p == '\n')
                            newLine++, newCol = 0;
                        else
                            newCol++;
                        break;
                    }
                    if (*p == '\n')
                        l++, c = 0;
                    else
                        c++;
                }
            }
            else if (pkt.character == 'D') { // LEFT
                if (newCol > 0)
                    newCol--;
                else if (newLine > 1) {
                    newLine--;
                    char buf[65536];
                    crdt_render(doc, buf, sizeof(buf));
                    int l = 1, colEnd = 0;
                    for (char *p = buf; *p; p++) {
                        if (l == newLine && *p == '\n') {
                            colEnd = cur_col;
                            break;
                        }
                        if (*p == '\n') {
                            if (l == newLine) {
                                colEnd = cur_col;
                                break;
                            }
                            l++, cur_col = 0;
                        }
                        else if (l == newLine)
                            cur_col++;
                    }
                    newCol = colEnd;
                }
            }

            // find the node at the new position to use as the new anchor
            CharNode* newAnchor = crdt_node_at(doc, newLine, newCol);
            struct Packet reply;
            memset(&reply, 0, sizeof(reply));
            reply.action = MOVE;
            reply.userSocket = pkt.userSocket;
            reply.timestamp = (long)time(NULL);
            reply.id = newAnchor->id;
            reply.character = pkt.character;
            send_packet(clientSocket, &reply);
            continue;
        }
        // SAVE
        if (pkt.action == SAVE) {
            struct Packet reply;
            memset(&reply, 0, sizeof(reply));
            reply.action = SAVE;
            reply.userSocket = clientSocket;
            reply.timestamp = (long)time(NULL);

            if (clientRole != ADMIN) {
                reply.id.counter = UINT32_MAX; // sentinel for DENIED
                send_packet(clientSocket, &reply);
                log_event("[Save] DENIED for socket %d ('%s'): Not Admin.\n", clientSocket, username);
            }
            else {
                pthread_mutex_lock(&lamport_lock);
                uint32_t snap_counter = g_lamport;
                pthread_mutex_unlock(&lamport_lock);
                save_document(doc, DOC_FILE, snap_counter);
                pthread_mutex_lock(&saveLock);
                lastSavedTime = (long)time(NULL);
                hasSaved = 1;
                pthread_mutex_unlock(&saveLock);
                reply.id.counter = 0; // sentinel for SUCCESS
                snprintf(reply.username, 32, "%s", username);
                broadcast_all(&reply);
                log_event("[Save] Document saved by admin '%s'.\n", username);
            }
            continue;
        }
        
        // DROP edits from viewers
        if (clientRole == VIEWER)
            continue;
        
        // INSERT
        if (pkt.action == INSERT) {
            // advance global Lamport clock to be >= sender's clock
            pthread_mutex_lock(&lamport_lock);
            if (pkt.id.counter > g_lamport)
                g_lamport = pkt.id.counter;
            g_lamport++;
            pthread_mutex_unlock(&lamport_lock);

            CharNode* inserted = crdt_insert(doc, pkt.leftID, pkt.id, pkt.character);
            if (!inserted) {
                log_event("[Server] INSERT failed: leftID not found. socket = %d\n", clientSocket);
                continue;
            }

            // broadcast to all other clients
            broadcast(&pkt, clientSocket);
            log_event("[Edit] INSERT '%c' id = (%u, %u) left = (%u, %u) by '%s'\n", pkt.character, pkt.id.counter, pkt.id.client_id, pkt.leftID.counter, pkt.leftID.client_id, username);
        }

        // DELETE
        else if (pkt.action == DELETE) {
            bool ok = crdt_delete(doc, pkt.id);
            if (!ok) {
                log_event("[Server] DELETE failed: ID not found. Socket = %d\n", clientSocket);
                continue;
            }
            broadcast(&pkt, clientSocket);
            log_event("[Edit] DELETE id = (%u, %u) by '%s'\n", pkt.id.counter, pkt.id.client_id, username);
        }
    }

    log_event("Client '%s' (socket %d) disconnected.\n", username, clientSocket);
    remove_client(clientSocket); // release semaphore
    close(clientSocket);
    pthread_exit(NULL);
}

int main() {
    doc = crdt_new();
    load_document(doc, DOC_FILE, &g_lamport); // load doc from disk
    register_sigint_handler(); // SIGINT setup

    // open write-end of the FIFO (blocks until logger opens the read-end)
    log_event("[Server] Opening FIFO '%s'... (start ./logger first)\n", FIFO_PATH);
    logFifoFd = open(FIFO_PATH, O_WRONLY);
    if (logFifoFd == -1)
        printf("[Server] WARNING: Could not open FIFO. Logging to stdout only.\n");
    else
        log_event("[Server] FIFO connected to logger.\n");

    // LINUX: use anonymous semaphore via sem_init (not named sem_open)
    clientSlots = malloc(sizeof(sem_t));
    if (sem_init(clientSlots, 0, MAX_CLIENTS) == -1) {
        perror("sem_init failed");
        exit(1);
    }
    log_event("[Server] Client semaphore initialized. Max clients: %d\n", MAX_CLIENTS);
    
    int serverSocket = initialize_server();

    log_event("[Server] Listening on port %d\n", PORT);
    
    while (!should_shutdown) {
        // semaphore wait blocks if MAX_CLIENTS already connected
        while (sem_trywait(clientSlots) == -1) {
            if (should_shutdown)
                goto shutdown;
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

        // role-based authentication
        AuthRequest authReq;
        ssize_t authBytes = read(clientSocket, &authReq, sizeof(AuthRequest));
        if (authBytes != (ssize_t)sizeof(AuthRequest)) {
            printf("[Auth] Incomplete auth from socket %d. Closing.\n", clientSocket);
            close(clientSocket);
            sem_post(clientSlots);
            continue;
        }

        // registration/authentication
        Role role;
        if (authReq.isRegister == 1)
            role = register_user(authReq.username, authReq.password);
        else
            role = authenticate(authReq.username, authReq.password);

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
        write_exact(clientSocket, &clientSocket, sizeof(int));
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
        send_packet(clientSocket, &joinPacket); // tell the client its own sync is complete

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

    // LINUX: destroy anonymous semaphore and free heap memory
    sem_destroy(clientSlots);
    free(clientSlots);
    crdt_free(doc);
    close(serverSocket);
    printf("[Server] Goodbye.\n");
    return 0;
}