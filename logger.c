// reads log messages from FIFO "/tmp/editor_log" and appends them to "server.log"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define FIFO_PATH "/tmp/editor_log"
#define LOG_FILE "server.log"
#define MSG_SIZE 256

int main() {
    if (mkfifo(FIFO_PATH, 0666) == -1 && errno != EEXIST) {
        perror("[Logger] mkfifo failed");
        exit(1);
    }
    printf("[Logger] FIFO '%s' ready. Waiting for server...\n", FIFO_PATH);

    // open read-end and block until server opens write-end
    int fifoFd = open(FIFO_PATH, O_RDONLY);
    if (fifoFd == -1) {
        perror("[Logger] Failed to open FIFO for reading");
        exit(1);
    }
    printf("[Logger] Connected. Writing logs to '%s'.\n", LOG_FILE);

    FILE *fp = fopen(LOG_FILE, "a");
    if (!fp) {
        perror("[Logger] Cannot open log file");
        close(fifoFd);
        exit(1);
    }

    // session start timestamp
    time_t now = time(NULL);
    fprintf(fp, "\nSESSION STARTED: %s", ctime(&now));
    fflush(fp);

    char msg[MSG_SIZE];
    ssize_t bytesRead;

    while ((bytesRead = read(fifoFd, msg, MSG_SIZE - 1)) > 0) {
        msg[bytesRead] = '\0';

        // remove trailing '\n'
        int len = strlen(msg);
        if (len > 0 && msg[len-1] == '\n')
            msg[len-1] = '\0';
        
        // timestamp every entry
        time_t t = time(NULL);
        struct tm *info;
        char buffer[80];

        // convert to local time structure
        info = localtime(&t);

        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", info);
        fprintf(fp, "[%s] %s\n", buffer, msg);
        fflush(fp);
    }

    now = time(NULL);
    fprintf(fp, "SESSION ENDED: %s\n", ctime(&now));
    fflush(fp);

    fclose(fp);
    close(fifoFd);
    unlink(FIFO_PATH);
    printf("[Logger] Server disconnected. Log written to '%s'. Goodbye.\n", LOG_FILE);
    return 0;
}