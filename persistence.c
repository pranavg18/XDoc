#include "persistence.h"

static void fcntl_lock(int fd, short lockType) {
    struct flock fl;
    fl.l_type = lockType; // F_RDLCK or F_WRLCK
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0; // lock the whole file
    fl.l_pid = getpid();

    if (fcntl(fd, F_SETLKW, &fl) == -1) {
        perror("[Persistence] fcntl lock failed");
        exit(1);
    }
}

static void fcntl_unlock(int fd) {
    struct flock fl;
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_SETLK, &fl) == -1)
        perror("[Persistence] fcntl unlock failed");
}

void load_document(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) { // file doesn't exist so start with a blank document
        printf("[Persistence] No existing document found. Starting fresh.\n");
        init_document();
        return;
    }

    // acquire shared read lock before reading
    fcntl_lock(fd, F_RDLCK);
    printf("[Persistence] Read lock acquired on '%s'\n", filename);

    FILE *fp = fdopen(fd, "r"); // allows us to use fgets later
    if (!fp) {
        perror("[Persistence] fdopen failed");
        fcntl_unlock(fd);
        close(fd);
        init_document();
        return;
    }
    
    LineNode *tail = NULL;
    char line[256];
    int lineNum = 1;

    while (fgets(line, sizeof(line), fp)) {
        int len = strlen(line);
        if (len > 0 && line[len-1] == '\n')
            line[len-1] = '\0';
        LineNode *node = create_node(lineNum, line);
        if (tail == NULL)
            documentHead = node;
        else {
            tail->next = node;
            node->prev = tail;
        }
        tail = node;
        lineNum++;
    }

    if (documentHead == NULL) // we still need 1 node if file was completely empty
        init_document();
    
    fcntl_unlock(fd);
    fclose(fp);
    printf("[Persistence] Document loaded from '%s' (%d lines).\n", filename, lineNum - 1);
}

void save_document(const char *filename) {
    // open for writing, create if missing, truncate if existing
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("[Persistence] Cannot open file for saving");
        return;
    }

    fcntl_lock(fd, F_WRLCK);
    printf("[Persistence] Write lock acquired on '%s'\n", filename);

    LineNode *node = documentHead;
    while (node) {
        // lock the line node so no client thread modifies it mid-save
        pthread_mutex_lock(&node->lock);

        int len = strlen(node->text);
        if (len > 0)
            write(fd, node->text, len);
        write(fd, "\n", 1);

        pthread_mutex_unlock(&node->lock);
        node = node->next;
    }

    fcntl_unlock(fd);
    close(fd);
    printf("[Persistence] Document saved to '%s'.\n", filename);
}