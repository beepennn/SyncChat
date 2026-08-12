Got it. We'll finish **only this milestone now**:

1. Add a proper `README.md`.
2. Set the GitHub repository description/topics shown in your screenshot.
3. Commit and push everything.
4. Verify the GitHub repository.
5. Stop here for today. We will **not start the username/broadcast milestone** yet.

## 1. GitHub repository description

In the GitHub dialog shown in your screenshot, I recommend:

**Description**

```text
A Linux-based multi-client chat and file sharing system implemented in C using TCP sockets, POSIX threads, mutex synchronization, Unix-domain IPC, and a custom syslog-like logging daemon.
```

**Website**

Leave it empty for now.

**Topics**

Use:

```text
network-programming
systems-programming
c
socket-programming
tcp
pthread
concurrency
ipc
unix-domain-sockets
file-sharing
linux
```

You do not need to add a deployment. Keep the repository focused on the source project.

---

# 2. Create the README

Use `vim`:

```bash
cd ~/SyncChat
vim README.md
```

Replace the entire file with:

````markdown
# SyncChat

**SyncChat: A Scalable Multi-Client Chat and File Sharing Server**

SyncChat is an undergraduate Network and Systems Programming project that demonstrates the implementation of a concurrent client-server network application using low-level Linux and POSIX programming interfaces.

The system is being developed in C on Linux and focuses on reliable TCP communication, concurrent client management, thread synchronization, inter-process communication, file I/O, file locking, and custom system-level logging.

## Project Overview

The system follows a centralized client-server architecture.

```text
                    +----------------------+
                    |      Clients         |
                    |----------------------|
                    | Terminal Interface   |
                    | TCP Communication    |
                    +----------+-----------+
                               |
                               | TCP
                               |
                    +----------v-----------+
                    |    Chat Server       |
                    |----------------------|
                    | Socket Management    |
                    | Pthread Handling     |
                    | Client Manager       |
                    | Message Routing      |
                    | File Manager         |
                    +----------+-----------+
                               |
                               | AF_UNIX
                               |
                    +----------v-----------+
                    | Custom Logger Daemon  |
                    |----------------------|
                    | Packet Validation    |
                    | Timestamping        |
                    | Log Formatting       |
                    | File Writing         |
                    +----------+-----------+
                               |
                               v
                         logs/server.log
````

## Core Technologies

* **Language:** C
* **Operating System:** Linux
* **Transport Protocol:** TCP
* **Network API:** POSIX/Berkeley sockets
* **Concurrency:** POSIX Threads (Pthreads)
* **Synchronization:** POSIX mutexes
* **IPC:** Unix-domain sockets (`AF_UNIX`)
* **File I/O:** Linux system calls
* **Build System:** GNU Make
* **Debugger:** GDB
* **Memory Analysis:** Valgrind
* **Network Analysis:** Wireshark
* **Version Control:** Git and GitHub

## Current Implementation

The following components have currently been implemented and tested:

* TCP server socket creation and configuration
* TCP client connection
* `socket()`, `bind()`, `listen()`, `accept()`, `connect()`
* `send()` and `recv()` handling
* Reliable `send_all()` and `recv_all()` operations
* Length-prefixed application-layer message framing
* Network byte order conversion
* Protocol message-type validation
* POSIX thread-based client handling
* Detached client threads
* Thread-safe client registry
* Mutex-protected shared client state
* Active client counting
* Custom Unix-domain logging daemon
* `AF_UNIX` / `SOCK_DGRAM` logging IPC
* Custom logging packet protocol
* Log validation and malformed packet rejection
* Custom timestamp generation
* Low-level log file writing using `open()` and `write()`
* Graceful signal handling using `sigaction()`
* Resource cleanup and restart handling
* Valgrind memory and file-descriptor verification

## Current Architecture

```text
Client Process
      |
      | TCP
      v
+----------------------+
|    Chat Server       |
|----------------------|
| Main Accept Thread   |
| Client Threads       |
| Client Registry      |
| Mutex Synchronization|
+----------+-----------+
           |
           | AF_UNIX
           v
+----------------------+
| Custom Logger Daemon |
+----------+-----------+
           |
           v
      server.log
```

## Project Structure

```text
SyncChat/
├── include/
│   ├── common/
│   │   ├── network_io.h
│   │   └── protocol.h
│   ├── logger/
│   │   └── logger_protocol.h
│   └── server/
│       ├── client_handler.h
│       ├── client_manager.h
│       └── server_config.h
│
├── src/
│   ├── client/
│   │   └── main.c
│   ├── common/
│   │   ├── network_io.c
│   │   └── protocol.c
│   ├── logger/
│   │   ├── main.c
│   │   └── test_client.c
│   └── server/
│       ├── main.c
│       ├── client_handler.c
│       └── client_manager.c
│
├── bin/
├── logs/
├── storage/
├── tests/
├── docs/
├── Makefile
├── README.md
└── .gitignore
```

## Build

Clone the repository and enter the project directory:

```bash
git clone git@github.com:beepennn/SyncChat.git
cd SyncChat
```

Build the complete project:

```bash
make clean
make
```

The following executables are generated:

```text
bin/logger_daemon
bin/logger_test
bin/syncchat_server
bin/syncchat_client
```

## Run the Current Implementation

### Start the logger daemon

```bash
./bin/logger_daemon
```

The logger listens on:

```text
/tmp/syncchat_logger.sock
```

and stores log records in:

```text
logs/server.log
```

### Start the server

In another terminal:

```bash
./bin/syncchat_server
```

The TCP server listens on port:

```text
9000
```

### Start a client

In another terminal:

```bash
./bin/syncchat_client
```

The current client can establish a TCP connection, receive the server greeting, and send a framed chat message.

## Logging Architecture

The project intentionally does not use the standard `syslog()` application API for its own logging.

Instead, a custom logging daemon is implemented:

```text
Application
     |
     | sendto()
     v
AF_UNIX / SOCK_DGRAM
     |
     v
Custom Logger Daemon
     |
     +-- Validate packet
     +-- Generate timestamp
     +-- Format entry
     +-- write()
     |
     v
logs/server.log
```

This component is inspired by the Linux syslog architecture and is implemented specifically to demonstrate system-level IPC and file management.

## Concurrency Model

The server uses a thread-per-client model.

```text
                 Main Server
                     |
                  accept()
                     |
        +------------+------------+
        |            |            |
        v            v            v
     Thread 1     Thread 2     Thread 3
     Client 1     Client 2     Client 3
```

A mutex protects the shared client registry from concurrent modification.

The mutex is held only while manipulating shared registry state; network operations are performed outside the critical section to avoid unnecessary serialization.

## Development Roadmap

The remaining implementation milestones include:

* Username registration and validation
* Persistent multi-user chat
* Broadcast messaging
* Private messaging
* Terminal-based user interface improvements
* Custom logging integration with the chat server
* File upload and download
* File locking
* Filename and path validation
* Connection failure recovery
* Custom performance timing module
* Concurrent stress testing
* Wireshark traffic analysis
* Valgrind and resource-leak testing
* Final performance evaluation
* Documentation and defense preparation

## Design Principles

The project emphasizes:

* Low-level Linux and POSIX APIs
* Explicit resource management
* Reliable TCP communication
* Message framing
* Concurrent execution
* Mutex-based synchronization
* Inter-process communication
* Defensive input validation
* Graceful error handling
* Resource cleanup
* Practical scalability

High-level networking frameworks and predefined logging services are intentionally avoided where the underlying systems concepts are part of the project's learning objectives.

## Academic Scope

This project is developed as part of the **Network and Systems Programming** course and focuses on practical implementation of:

* TCP socket programming
* Client-server architecture
* POSIX threads
* Thread synchronization
* Inter-process communication
* File I/O
* File locking
* Concurrent server design
* Linux system programming
* Network debugging and performance analysis

## Project Status

**Development Status:** In Progress

The networking foundation, concurrent client handling, thread-safe client registry, and custom logging daemon have been implemented and tested. Higher-level chat and file-sharing functionality is under active development.

## Author

**Bipin Lamsal**

GitHub: [@beepennn](https://github.com/beepennn)

````
