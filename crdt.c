#include "crdt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// internal hash-map helpers
static uint32_t map_hash(ID id) {
    uint32_t h = id.counter * 2654435761u ^ ((uint32_t)id.client_id * 2246822519u);
    return h % CRDT_MAP_BUCKETS;
}

// insert into the hash map
static void map_put(CRDTDoc* doc, CharNode* node) {
    uint32_t h = map_hash(node->id);
    MapEntry* ent = (MapEntry *)malloc(sizeof(MapEntry));
    if (!ent) {
        perror("crdt map_put malloc");
        exit(1);
    }
    ent->id = node->id;
    ent->node = node;
    ent->next = doc->map[h];
    doc->map[h]= ent;
}

// lookup by ID
CharNode* crdt_find(CRDTDoc* doc, ID id) {
    uint32_t h = map_hash(id);
    MapEntry* ent = doc->map[h];
    while (ent) {
        if (id_eq(ent->id, id))
            return ent->node;
        ent = ent->next;
    }
    return NULL;
}

CRDTDoc* crdt_new(void) {
    CRDTDoc* doc = (CRDTDoc *)calloc(1, sizeof(CRDTDoc));
    if (!doc) {
        perror("crdt_new");
        exit(1);
    }
    pthread_mutex_init(&doc->lock, NULL);

    // sentinel head node ID {0, 0}
    CharNode* sentinel = (CharNode *)calloc(1, sizeof(CharNode));
    if (!sentinel) {
        perror("crdt sentinel");
        exit(1);
    }
    sentinel->id = id_zero();
    sentinel->leftID = id_zero();
    sentinel->value = 0;
    sentinel->deleted = false;
    sentinel->prev = NULL;
    sentinel->next = NULL;

    doc->head = sentinel;
    map_put(doc, sentinel);
    return doc;
}

void crdt_free(CRDTDoc* doc) {
    if (!doc)
        return;
    
    // free hash map
    for (int i = 0; i < CRDT_MAP_BUCKETS; i++) {
        MapEntry* e = doc->map[i];
        while (e) {
            MapEntry* next = e->next;
            free(e);
            e = next;
        }
    }

    // free linked list
    CharNode* n = doc->head;
    while (n) {
        CharNode* nx = n->next;
        free(n);
        n = nx;
    }
    pthread_mutex_destroy(&doc->lock);
    free(doc);
}

/* INSERT
We skip a right neighbour if:
    1. Its leftID is the same as our leftID (it's a direct sibling), AND
    2. Its ID is greater than ours (it outranks us).
        OR
    3. Its leftID is different (it's a child of one of the siblings we already skipped i.e. part of a
       "sibling subtree"), in which case we must also skip it to keep the whole subtree together.
*/
CharNode* crdt_insert(CRDTDoc* doc, ID leftID, ID newID, char value) {
    pthread_mutex_lock(&doc->lock);

    CharNode* anchor = crdt_find(doc, leftID);
    if (!anchor) {
        pthread_mutex_unlock(&doc->lock);
        return NULL; // leftID is not in document
    }

    // build the new node
    CharNode* node = (CharNode *)calloc(1, sizeof(CharNode));
    if (!node) {
        perror("crdt_insert malloc");
        exit(1);
    }
    node->id = newID;
    node->leftID = leftID;
    node->value = value;
    node->deleted = false;

    /*
    Walk right from the anchor, skipping nodes that outrank us
    Track skip_depth to know when we have exited a sibling's subtree
        1. We increment it when we start skipping a direct sibling
        2. We decrement it when we pass a node whose leftID is "below" the skip zone
    
    */
    CharNode* pos = anchor; // we will insert after pos
    CharNode* cur = anchor->next;

    // collect the IDs of all direct siblings we decide to skip
    ID skipped[64]; // 64 concurrent writers
    int nskip = 0;

    while (cur) {
        bool is_direct_sibling = id_eq(cur->leftID, leftID); // is cur a direct sibling (anchored at the same leftID)?
        bool in_skip_subtree = false; // is cur in the subtree of a node we have already skipped?

        for (int i = 0; i < nskip; i++) {
            if (id_eq(cur->leftID, skipped[i])) {
                in_skip_subtree = true;
                break;
            }
        }
        // if cur's leftID was itself skipped, add cur->id to the skip set so its children are also skipped
        if (in_skip_subtree && nskip < 64) {
            skipped[nskip++] = cur->id;
            pos = cur;
            cur = cur->next;
            continue;
        }

        if (is_direct_sibling && id_cmp(cur->id, newID) > 0) {
            // this sibling outranks us so skip it and remember its ID
            if (nskip < 64) skipped[nskip++] = cur->id;
            pos = cur;
            cur = cur->next;
            continue;
        }

        break; // cur does not need to be skipped since it is our insertion point
    }

    // splice newNode between pos and pos->next
    node->prev = pos;
    node->next = pos->next;
    if (pos->next)
        pos->next->prev = node;
    pos->next = node;

    map_put(doc, node);
    pthread_mutex_unlock(&doc->lock);
    return node;
}

// delete (Tombstone)
bool crdt_delete(CRDTDoc* doc, ID targetID) {
    pthread_mutex_lock(&doc->lock);
    CharNode* node = crdt_find(doc, targetID);
    if (!node || id_eq(node->id, id_zero())) {
        pthread_mutex_unlock(&doc->lock);
        return false;
    }
    node->deleted = true;
    pthread_mutex_unlock(&doc->lock);
    return true;
}

// rendering
size_t crdt_render(CRDTDoc* doc, char *out_buf, size_t buf_size) {
    if (buf_size == 0)
        return 0;
    pthread_mutex_lock(&doc->lock);

    size_t pos = 0;
    CharNode* cur = doc->head->next; // skip sentinel
    while (cur && pos < buf_size - 1) {
        if (!cur->deleted)
            out_buf[pos++] = cur->value;
        cur = cur->next;
    }
    out_buf[pos] = '\0';
    pthread_mutex_unlock(&doc->lock);
    return pos;
}

// positional helpers
CharNode* crdt_node_at(CRDTDoc* doc, int line, int col) {
    pthread_mutex_lock(&doc->lock);

    int cur_line = 1, cur_col = 0;
    CharNode* result = doc->head; // default: before everything
    CharNode* lastLive = doc->head; // track last live node
    CharNode* cur = doc->head->next;

    while (cur) {
        if (!cur->deleted) {
            if (cur_line == line && cur_col == col) {
                result = cur->prev; // anchor is the node before this position
                break;
            }
            if (cur->value == '\n'){
                cur_line++;
                cur_col = 0;
            }
            else
                cur_col++;
            lastLive = cur; // update tracker
        }
        cur = cur->next;
    }

    // if we exhausted the list at exactly the right position
    if (!cur && cur_line == line && cur_col == col)
        result = lastLive; // use last live node
    pthread_mutex_unlock(&doc->lock);
    return result;
}

void crdt_pos_of(CRDTDoc* doc, ID anchorID, int* out_line, int* out_col) {
    pthread_mutex_lock(&doc->lock);

    // sentinel means cursor is at the very start
    if (id_eq(anchorID, id_zero())) {
        *out_line = 1;
        *out_col = 0;
        pthread_mutex_unlock(&doc->lock);
        return;
    }

    int cur_line = 1, cur_col = 0;
    CharNode* cur = doc->head->next;
    while (cur) {
        if (id_eq(cur->id, anchorID))
            break;
        if (!cur->deleted) {
            if (cur->value == '\n') {
                cur_line++;
                cur_col = 0;
            }
            else
                cur_col++;
        }
        cur = cur->next;
    }
    
    *out_line = cur_line;
    *out_col = cur_col;
    pthread_mutex_unlock(&doc->lock);
}