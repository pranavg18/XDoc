#include "terminal.h"

struct termios originalTermios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTermios);
}

// turns off line-buffering
void enable_raw_mode() {
    // save a backup of current terminal settings
    tcgetattr(STDIN_FILENO, &originalTermios);
    atexit(disable_raw_mode); // restore on exit

    struct termios raw = originalTermios;

    raw.c_iflag &= ~(IXON | IXOFF); // disables Ctrl+S/Ctrl+Q flow control on macOS
    raw.c_lflag &= ~(ECHO | ICANON | ISIG); // flip the bits controlling canonical mode and echo and SIG

    // the timeout is that read() will wait for 1 byte or return 0 after 100ms
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // apply the new settings to the terminal
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}