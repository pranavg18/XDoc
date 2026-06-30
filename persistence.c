#include "persistence.h"
#include <errno.h>

// On-disk record layout
typedef struct __attribute__((packed)) {
    uint32_t counter;
    uint16_t client_id;
    uint32_t left_counter;
    uint16_t left_client_id;
    char value;
    uint8_t deleted;
} DiskRecord;

// header at the very start of the file
typedef struct __attribute__((packed)) {
    uint32_t magic; // 0xCRDT1234
    uint32_t max_counter; // highest Lamport counter seen
    uint32_t num_records;
} DiskHeader;

#define DISK_MAGIC 0xCDEF1234u

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

void load_document(CRDTDoc* doc, const char *filename, uint32_t* max_counter) {
    *max_counter = 0;

    int fd = open(filename, O_RDONLY);
    if (fd == -1) { // file doesn't exist so start with a blank document
        printf("[Persistence] No existing document found. Starting fresh.\n");
        return;
    }

    // acquire shared read lock before reading
    fcntl_lock(fd, F_RDLCK);
    printf("[Persistence] Read lock acquired on '%s'\n", filename);

    DiskHeader hdr;
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) || hdr.magic != DISK_MAGIC) {
        fprintf(stderr, "[Persistence] Corrupt or empty document file. Starting fresh.\n");
        fcntl_unlock(fd);
        close(fd);
        return;
    }
    *max_counter = hdr.max_counter;

    for (uint32_t i = 0; i < hdr.num_records; i++) {
        DiskRecord rec;
        if (read(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec)) {
            fprintf(stderr, "[Persistence] Truncated record at index %u. Stopping.\n", i);
            break;
        }
        ID newID = {rec.counter, rec.client_id};
        ID leftID = {rec.left_counter, rec.left_client_id};

        // replay the insert into the CRDT
        CharNode* n = crdt_insert(doc, leftID, newID, rec.value);
        if (!n) {
            fprintf(stderr, "[Persistence] WARNING: Could not replay record %u (leftID not found).\n", i);
            continue;
        }
        if (rec.deleted)
            crdt_delete(doc, newID);
    }
    fcntl_unlock(fd);
    fclose(fd);
    printf("[Persistence] Document loaded from '%s' (max_counter = %u).\n", filename, *max_counter);
}

void save_document(CRDTDoc* doc, const char *filename, uint32_t max_counter) {
    // open for writing, create if missing, truncate if existing
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("[Persistence] Cannot open file for saving");
        return;
    }

    fcntl_lock(fd, F_WRLCK);
    printf("[Persistence] Write lock acquired on '%s'\n", filename);

    // count records first (skip sentinel)
    pthread_mutex_lock(&doc->lock);
    uint32_t num_records = 0;
    CharNode* cur = doc->head->next;
    while (cur) {
        num_records++;
        cur = cur->next;
    }

    DiskHeader hdr;
    hdr.magic = DISK_MAGIC;
    hdr.max_counter = max_counter;
    hdr.num_records = num_records;
    write(fd, &hdr, sizeof(hdr));

    cur = doc->head->next;
    while (cur) {
        DiskRecord rec;
        rec.counter = cur->id.counter;
        rec.client_id = cur->id.client_id;
        rec.left_counter = cur->leftID.counter;
        rec.left_client_id = cur->leftID.client_id;
        rec.value = cur->value;
        rec.deleted = (uint8_t)cur->deleted;
        write(fd, &rec, sizeof(rec));
        cur = cur->next;
    }
    pthread_mutex_unlock(&doc->lock);

    fcntl_unlock(fd);
    close(fd);
    printf("[Persistence] Document saved to '%s' (%u records).\n", filename, num_records);
}