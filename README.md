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

If your system uses macOS then delete the Makefile_linux and use only Makefile_mac.
If your system uses Linux then delete the Makefile_mac and use only Makefile_linux.