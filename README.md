# Real-Time Collaborative Terminal Text Editor

A multithreaded, real-time collaborative text editor built entirely in C that runs natively in the Unix/Linux terminal. It functions similarly to a terminal-based Google Docs, allowing multiple clients to connect to a centralized server and edit a shared document simultaneously.

## Features

* **Real-Time Collaboration:** Multiple users can type, delete, and navigate the document simultaneously with synchronization.
* **Server-Side Cursor Engine:** Smart cursor tracking ensures your cursor moves correctly even when other users type on or delete the lines around you (OT-style syncing).
* **Role-Based Access Control:** Users log in with credentials and are assigned roles:
  * **Admin:** Can read, edit, and save the document to disk.
  * **Editor:** Can read and edit the document.
  * **Viewer:** Read-only access.
* **Concurrency & Synchronization:** Uses POSIX threads (`pthread`) for non-blocking client handling, fine-grained mutex locks per line to prevent race conditions, and semaphores to cap active connections.
* **Persistent Storage:** Admins can save the document state to `document.txt`. The server automatically loads this file on startup.
* **Dedicated IPC Logging:** A standalone `logger` process captures server events in real-time via a Named Pipe (FIFO) and writes them to `server.log`.
* **Hardware-Accelerated UI:** Uses raw terminal mode (`termios`) and ANSI escape sequences to redraw the UI efficiently without clearing the entire screen.

## Project Structure

* **`server_main_linux.c` / `server.c`:** Core server loop, connection acceptance, and broadcasting.
* **`client_main.c` / `client.c`:** Client-side keyboard polling, network listening, and ANSI UI rendering.
* **`document.c`:** Doubly Linked List implementation for text storage and manipulation (insert, delete, split line, merge lines).
* **`auth.c`:** Parses `users.txt` to authenticate connections and assign roles.
* **`persistence.c`:** Handles POSIX file locking (`fcntl`) and I/O to save/load `document.txt`.
* **`logger.c`:** Standalone process that reads from `/tmp/editor_log` FIFO.
* **`terminal_linux.c`:** Modifies terminal attributes to enable raw keystroke capture.

## Setting Up

### Delete one Makefile
If your system uses macOS then run `rm Makefile_linux` folllowed by `mv Makefile_mac Makefile`.

If your system uses macOS then run `rm Makefile_mac` folllowed by `mv Makefile_linux Makefile`.

### Communication between multiple devices (macOS)
1. **Find your local IP:** Open your Mac terminal and run `ifconfig` (or `ipconfig getifaddr en0`). Look for an IPv4 address that probably starts with `192.168.` or `10.0.`. Suppose it is `192.168.1.15`.
2. **Update the Client:** Change the definition in `client.h` to `#define IP "192.168.1.15"`.
3. **Recompile:** Run `make all` to recompile the client with the new IP address.
4. **Connect:** Run `./server` on your Mac. Send the compiled `./client` executable to your friend on the same Wi-Fi network. When they run it, their packets will route through your home router directly to your Mac.

### Communication between multiple devices (Linux)
1. **Find your local IP:** Open your Linux terminal and run `hostname -I`. Look for an IPv4 address that probably starts with `192.168.` or `10.0.`. Suppose it is `192.168.1.15`.
2. **Update the Client:** Change the definition in `client.h` to `#define IP "192.168.1.15"`.
3. **Recompile:** Run `make all` to recompile the client with the new IP address.
4. **Connect:** Run `./server` on your Linux machine. Send the compiled `./client` executable to your friend on the same Wi-Fi network. When they run it, their packets will route through your home router directly to your machine.
