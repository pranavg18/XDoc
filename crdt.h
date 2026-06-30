#ifndef CRDT_H
#define CRDT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "protocol.h" // for the ID type

/* One character in the RGA (Replicated Growable Array) CRDT sequence
Every character lives in this flat doubly-inked list.
Deleted characters stay as tombstones (deleted = true) so characters that reference them as a leftID can still find an anchor
Ordering rule for concurrent inserts after the same anchor:
    When inserting new node N after anchor A, walk forward past any existing right-siblings of A whose ID
    is greater than N's ID (higher Lamport counter wins and client_id breaks those ties).
*/

typedef struct CharNode {
    ID id; // this character's unique Lamport ID
    ID leftID; // its anchor's ID
    char value; // the actual character
    bool deleted; // tombstone flag
    struct CharNode* prev;
    struct CharNode* next;
} CharNode;

// Hash-Map bucket for O(1) lookup of a CharNode by its ID
#define CRDT_MAP_BUCKETS 4096 // Simple chained hash table with 4096 buckets

typedef struct MapEntry {
    ID id;
    CharNode* node;
    struct MapEntry* next;
} MapEntry;

// Document state is a flat doubly-linked list with a hash map
typedef struct {
    CharNode* head; // sentinel head node (id = {0, 0}, value = 0)
    MapEntry* map[CRDT_MAP_BUCKETS];
    pthread_mutex_t lock;
} CRDTDoc;

// initialize an empty document with a sentinel head node
CRDTDoc* crdt_new(void);

// free all memory owned by the document
void crdt_free(CRDTDoc* doc);

// insert a character {newID, value} after anchor leftID
CharNode* crdt_insert(CRDTDoc* doc, ID leftID, ID newID, char value); // returns the inserted CharNode on success, and NULL if leftID is not found

// mark the node with targetID as deleted (tombstone)
bool crdt_delete(CRDTDoc* doc, ID targetID); // return true on success, false if not found

/*
render the live (non-tombstoned sequence) into out_buf
buf_size is the max no. of bytes to write
*/
size_t crdt_render(CRDTDoc* doc, char* out_buf, size_t buf_size); // returns the no. of bytes written

// look up a CharNode by its ID
CharNode* crdt_find(CRDTDoc* doc, ID id); // returns NULL if not found

/*
walk the live sequence and return the CharNode at (line, col)
used for mapping arrow-key movement back to an ID
*/
CharNode* crdt_node_at(CRDTDoc* doc, int line, int col); // returns the sentinel head node if the position is before all content

// given a CharNode, compute its current screen position
void crdt_pos_of(CRDTDoc* doc, ID anchorID, int* out_line, int* out_col);

#endif // CRDT_H