#include "document.h"

LineNode* documentHead = NULL;

// initialize empty document
void init_document() {
    documentHead = create_node(1, "");
    printf("Document is initialized in memory.\n");
}

// create a new line
LineNode* create_node(int lineNumber, const char* text) {
    LineNode* newNode = (LineNode *)malloc(sizeof(LineNode));
    if (!newNode) {
        perror("Memory allocation failed");
        exit(1);
    }

    newNode->lineNumber = lineNumber;
    memset(newNode->text, 0, sizeof(newNode->text)); // clear garbage memory
    if (text)
        strncpy(newNode->text, text, 255);
    
    // initialize mutex for this line
    pthread_mutex_init(&newNode->lock, NULL);
    
    newNode->prev = NULL;
    newNode->next = NULL;

    return newNode;
}

void split_line(LineNode *currentLine, int splitIndex) {
    // assume that currentLine is already locked by the server_main loop
    int oldLength = strlen(currentLine->text);

    // create new line node
    LineNode *newLine = create_node(currentLine->lineNumber + 1, "");

    // string slice
    if (splitIndex < oldLength) {
        // copy right half into new line
        strcpy(newLine->text, currentLine->text + splitIndex);
        
        // truncate original line
        currentLine->text[splitIndex] = '\0';
    }

    // insert new line betweeen currentLine and currentLine->next
    newLine->next = currentLine->next;
    newLine->prev = currentLine;

    if (currentLine->next) {
        pthread_mutex_lock(&currentLine->next->lock);
        currentLine->next->prev = newLine;
        pthread_mutex_unlock(&currentLine->next->lock);
    }
    currentLine->next = newLine;

    // update line numbers for rest of the doc
    LineNode *temp = newLine->next;
    while (temp) {
        pthread_mutex_lock(&temp->lock);
        temp->lineNumber++;
        pthread_mutex_unlock(&temp->lock);
        temp = temp->next;
    }
}

void merge_lines(LineNode *currentLine) {
    LineNode *prevLine = currentLine->prev;

    // append current text to end of previous line
    int prevLen = strlen(prevLine->text);
    int available = 255 - prevLen;
    if (available > 0)
        strcat(prevLine->text, currentLine->text);

    // unlink current line
    prevLine->next = currentLine->next;
    if (currentLine->next) {
        pthread_mutex_lock(&currentLine->next->lock);
        currentLine->next->prev = prevLine;
        pthread_mutex_unlock(&currentLine->next->lock);
    }

    // shift all subsequent line numbers
    LineNode *temp = prevLine->next;
    while (temp) {
        pthread_mutex_lock(&temp->lock);
        temp->lineNumber--;
        pthread_mutex_unlock(&temp->lock);
        temp = temp->next;
    }
}