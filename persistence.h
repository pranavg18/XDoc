#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "crdt.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define DOC_FILE "document.bin"

/* Load document from disk into CRDT
Each record is a fixed-style tuple: {ID id, ID leftID, char value, uint8_t deleted}
Also loads and restores max_counter so the server resumes issuing fresh, non-colliding Lamport clock values
after a restart.
*/
void load_document(CRDTDoc* doc, const char *filename, uint32_t* max_counter);

/* Write the full CRDT sequence to disk in list order so load_document can replay inserts in the correct
sequence to reconstruct the exact list.
*/
void save_document(CRDTDoc* doc, const char *filename, uint32_t max_counter);

#endif // PERSISTENCE_H