#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "document.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define DOC_FILE "document.txt"

// load document.txt into linked list when server starts up
void load_document(const char *filename);

// write every line of the linked list to disk
void save_document(const char *filename);

#endif