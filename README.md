# Real-Time Collaborative Terminal Text Editor

A multithreaded, real-time collaborative text editor built entirely in C that runs natively in the Unix/Linux terminal. It functions similarly to a terminal-based Google Docs, allowing multiple clients to connect to a centralized server and edit a shared document simultaneously.

## Features

* **Real-Time Collaboration:** Multiple users can type, delete, and navigate the document simultaneously with synchronization.
* **CRDT-Based Synchronization:** Uses a Replicated Growable Array (RGA) Conflict-Free Replicated Data Type (CRDT) with Lamport timestamps to ensure eventual consistency and accurate cursor tracking during concurrent edits.
* **Role-Based Access Control:** Users log in with credentials and are assigned roles:
  * **Admin:** Can read, edit, and save the document to disk.
  * **Editor:** Can read and edit the document.
  * **Viewer:** Read-only access.
* **Concurrency & Synchronization:** Uses POSIX threads (`pthread`) for non-blocking client handling, fine-grained mutex locks per line to prevent race conditions, and semaphores to cap active connections.
* **Persistent Storage:** Admins can save the document state to `document.bin`. The server automatically loads this binary record on startup to rebuild the CRDT sequence.
* **Dedicated IPC Logging:** A standalone `logger` process captures server events in real-time via a Named Pipe (FIFO) and writes them to `server.log`.
* **Hardware-Accelerated UI:** Uses raw terminal mode (`termios`) and ANSI escape sequences to redraw the UI efficiently without clearing the entire screen.

## Project Structure

* **`server_main_linux.c` / `server_main_mac.c` / `server.c`:** Core server loop, connection acceptance, and broadcasting.
* **`client_main.c` / `client.c`:** Client-side keyboard polling, network listening, local CRDT replica management, and ANSI UI rendering.
* **`crdt.c`:** CRDT implementation using a doubly linked list with tombstones for deletions, and a hash map for fast O(1) ID lookups.
* **`auth.c`:** Parses `users.txt` to authenticate connections and assign roles.
* **`persistence.c`:** Handles POSIX file locking (`fcntl`) and I/O to save/load the CRDT sequence in binary format (`document.bin`).
* **`logger.c`:** Standalone process that reads from `/tmp/editor_log` FIFO.
* **`terminal_linux.c` / `terminal_mac.c`:** Modifies terminal attributes to enable raw keystroke capture.

## Setting Up

### Delete one Makefile
If your system uses macOS then run `rm Makefile_linux` followed by `mv Makefile_mac Makefile`.

If your system uses macOS then run `rm Makefile_mac` followed by `mv Makefile_linux Makefile`.

### Communication between multiple devices (macOS)
1. **Find the Server IP:** Open your terminal on the machine running the server and find its local IP address (run `ifconfig` (or `ipconfig getifaddr en0`) on macOS or `hostname -I` on Linux). Look for an IPv4 address that probably starts with `192.168.` or `10.0.`. Suppose it is `192.168.1.15`.
2. **Start the Server:** Run `./server` on that machine.
3. **Connect a Client:** Send the compiled `./client` executable to a friend on the same Wi-Fi network (or run it yourself on a different machine). To connect it to the server, simply pass the server's IP address as a command-line argument:
   ```bash
   ./client 192.168.1.15
