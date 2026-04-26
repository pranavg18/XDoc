#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

// line of text
typedef struct LineNode {
    int lineNumber;
    char text[256];

    // lock for just this line
    pthread_mutex_t lock;

    struct LineNode *prev;
    struct LineNode *next;
} LineNode;

// global pointer to start of document
extern LineNode *documentHead;

void init_document();
LineNode* create_node(int lineNumber, const char* text);
void split_line(LineNode *currentLine, int splitIndex);
void merge_lines(LineNode *currentLine);

#endif