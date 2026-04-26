#ifndef TERMINAL_H
#define TERMINAL_H

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

void disable_raw_mode();
void enable_raw_mode();

#endif