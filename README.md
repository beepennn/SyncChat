# SyncChat

**SyncChat: A Scalable Multi-Client Chat and File Sharing Server**

SyncChat is an undergraduate **Network and Systems Programming** project implemented in C on Linux. It demonstrates reliable TCP communication, application-layer framing, multi-client concurrency with POSIX threads, synchronization with mutexes and Linux `fcntl()` file locks, low-level file I/O, Unix-domain socket IPC, custom logging, defensive file handling, performance measurement, and network/resource analysis.

The current backend and command-line client are feature-complete for the core chat and file-sharing requirements. A Linux GTK client can be added as a presentation interface without changing the underlying TCP protocol.

## Features

- Multi-client TCP server using a thread-per-client architecture
- Username registration with duplicate-name rejection
- Public broadcast messaging
- Private messaging with `/msg`
- Online-user listing with `/users`
- Shared-file listing with `/files`
- Binary-safe file upload and download
- Fixed-size streaming blocks for file transfers
- Per-client send serialization to protect each TCP byte stream
- Shared/exclusive file locking for concurrent file access
- Filename validation and directory-traversal protection
- Symbolic-link defenses for server-side shared files
- Interrupted-transfer cleanup and protocol resynchronization
- Custom syslog-like logger daemon over Unix-domain datagram sockets
- Monotonic-clock file-transfer timing and application throughput reporting
- Automated concurrency and stress testing
- Valgrind memory/file-descriptor verification
- Wireshark protocol evidence

## Architecture

```text
                              TCP port 9000
+------------------+        framed control         +-------------------------+
| SyncChat Client  | <---------------------------> |     SyncChat Server     |
|------------------|                               |-------------------------|
| Input/send path  |                               | Main accept thread      |
| Receiver thread  |                               | Thread per client       |
| Upload state     |                               | Client manager          |
| Download handler |                               | Message routing         |
+------------------+                               | File manager            |
                                                   | File locking            |
                                                   | Performance timer       |
                                                   +------------+------------+
                                                                |
                                                                | AF_UNIX
                                                                | SOCK_DGRAM
                                                                v
                                                   +-------------------------+
                                                   | Custom Logger Daemon    |
                                                   |-------------------------|
                                                   | Packet validation       |
                                                   | Timestamping            |
                                                   | Low-level file writing  |
                                                   +------------+------------+
                                                                |
                                                                v
                                                        logs/server.log
```

The server listens on `0.0.0.0:9000`. Each authenticated client is handled by a detached POSIX thread. The server currently supports up to `64` registered clients.

## Application Protocol

TCP provides an ordered byte stream but does not preserve application message boundaries. SyncChat therefore uses an explicit application-layer frame:

```text
+----------------------+----------------------+-------------------+
| 32-bit message type  | 32-bit payload size | payload bytes     |
+----------------------+----------------------+-------------------+
        4 bytes                 4 bytes
```

The two header fields are transmitted in network byte order using `htonl()` and decoded using `ntohl()`.

Important protocol limits:

```text
Maximum framed payload: 4096 bytes
Maximum username size:  31 characters + terminator
Maximum shared filename: 127 characters + terminator
File transfer block:    8192 bytes
Maximum shared file:    50 MiB
Maximum clients:        64
```

Current message types include login, public chat, disconnect, upload, download, file-list request, generic response/error, broadcast, user list, private message, and individual file-list entries.

## Reliable TCP I/O

SyncChat does not assume that one `send()` or `recv()` transfers the requested amount. The common networking layer provides `send_all()` and `recv_all()` loops that handle partial transfers and interrupted system calls.

Server-side socket writes are also synchronized per destination client. Every registered client owns a `send_mutex`. Public broadcasts, private messages, responses, file-list messages, and file-transfer streams use that synchronization so two server threads cannot corrupt the same TCP byte stream.

The lock order used by the client manager is:

```text
clients_mutex -> destination send_mutex
```

The global registry mutex is released before potentially blocking network I/O whenever the destination stream has been safely serialized.

## Chat Operations

After connecting, the client registers a username. Usernames are validated and duplicate active names are rejected atomically while the client registry is locked.

The CLI commands are:

```text
/users
/files
/msg <user> <message>
/upload <local-path>
/download <filename>
/quit
```

Text that does not begin with one of these commands is sent as a public chat message.

The asynchronous client receiver thread allows broadcasts and private messages to arrive while the main thread waits for user input.

## File Upload Protocol

The upload protocol uses a framed control transition followed by an exact-length raw byte stream.

```text
Client                                      Server
  |                                           |
  | MSG_UPLOAD "filename|size"                |
  |------------------------------------------>|
  |                                           | validate metadata/path
  |                                           | acquire exclusive file lock
  |                                           | create hidden temp file
  | MSG_RESPONSE "UPLOAD_READY"               |
  |<------------------------------------------|
  |                                           |
  | exactly <size> raw bytes                  |
  |==========================================>|
  |                                           | fsync/fchmod/close
  |                                           | atomically publish file
  | MSG_RESPONSE "UPLOAD_SUCCESS ..."         |
  |<------------------------------------------|
```

Incomplete uploads are written only to hidden temporary files. A completed upload is atomically published without overwriting an existing shared file. Duplicate filenames are rejected.

If local server-side writing fails after the transfer has started, the server continues draining the exact promised byte count where possible so framed protocol synchronization is preserved. If the TCP transfer itself fails, the incomplete temporary file is discarded and the connection is treated as unusable.

## File Download Protocol

Downloads use a framed metadata message followed by an exact raw byte count:

```text
Client                                      Server
  |                                           |
  | MSG_DOWNLOAD "filename"                   |
  |------------------------------------------>|
  |                                           | validate/open
  |                                           | acquire shared file lock
  | MSG_RESPONSE                              |
  | "DOWNLOAD_READY filename|size"            |
  |<------------------------------------------|
  |                                           |
  | exactly <size> raw bytes                  |
  |<==========================================|
```

The server holds the requesting client's `send_mutex` across the `DOWNLOAD_READY` frame and the complete raw file stream. This prevents a broadcast, private message, or other framed response from being inserted into the middle of binary file data.

The client receiver thread is the sole socket reader. When it recognizes `DOWNLOAD_READY`, it immediately consumes exactly the advertised number of bytes. The downloaded data is written to a hidden temporary file and published locally only after successful completion.

## File Listing

`/files` does not place the entire directory listing into one 4096-byte frame. Instead, the server sends a typed sequence:

```text
MSG_RESPONSE  "FILE_LIST_BEGIN"
MSG_FILELIST  "filename|size"
MSG_FILELIST  "filename|size"
...
MSG_RESPONSE  "FILE_LIST_END <count>"
```

Only completed, valid regular shared files are exposed. Hidden temporary objects, the lock directory, and unsafe filesystem entries are excluded.

This protocol is suitable for both the CLI and a future graphical shared-files panel.

## File Locking and Synchronization

SyncChat uses Linux open-file-description locks through `fcntl()` for per-filename advisory coordination.

```text
Download/read  -> shared lock
Upload/write   -> exclusive lock
```

Multiple downloads of the same file may proceed concurrently. An upload for that filename requires exclusive access. Operations on different filenames remain independent instead of being serialized by one global storage mutex.

Lock coordination objects are kept in a hidden storage lock directory and are never exposed through `/files`.

## File Security

Shared filenames are treated as basenames, not arbitrary paths. The validation policy rejects hidden names, path separators, `..` sequences, invalid characters, and names outside the configured size limit.

The hardened server also uses directory-relative file operations for shared storage, together with mechanisms such as `openat()`, `O_NOFOLLOW`, and `fstatat(..., AT_SYMLINK_NOFOLLOW)` where appropriate. This reduces dependence on string filtering alone and prevents symbolic-link entries from being treated as normal downloadable shared files.

Server-side tests cover directory traversal attempts, malformed upload metadata, symbolic-link downloads, interrupted uploads, duplicate files, invalid file requests, and continued operation after rejected requests.

## Custom Logger Daemon

SyncChat intentionally does **not** use the standard `syslog()` application API.

The server sends structured log packets to a custom logger daemon:

```text
SyncChat Server
      |
      | sendto()
      v
AF_UNIX / SOCK_DGRAM
/tmp/syncchat_logger.sock
      |
      v
Custom Logger Daemon
      |
      +-- validate magic/version/level
      +-- add timestamp and sender PID
      +-- format record
      +-- write() to append-only log
      |
      v
logs/server.log
```

Each server log call uses a short-lived Unix-domain datagram socket. Logging is best-effort: failure to reach the logger daemon must not terminate or disrupt the chat service.

Because one logger daemon process performs the final logfile writes, server client threads do not directly share or mutex-protect the logfile.

Logged events include connections, username registration outcomes, chat operations, file operations, transfer failures, security rejections, disconnections, runtime errors, and performance measurements.

## Performance Timing

File-transfer timing uses `clock_gettime(CLOCK_MONOTONIC)` so elapsed measurements are not affected by wall-clock adjustments.

For successful transfers, SyncChat records:

```text
bytes
elapsed seconds
application throughput in MiB/s
```

The reported value is **application-level transfer throughput**, not raw physical-link bandwidth. It includes operating-system scheduling, socket, protocol, and application overhead.

Example log form:

```text
[PERFORMANCE] DOWNLOAD username=Alice filename=perf-test.bin \
bytes=10485760 seconds=... throughput_mib_s=...
```

## Build

Requirements include GCC, GNU Make, POSIX threads, and a Linux environment.

Build everything:

```bash
make clean
make
```

Generated binaries:

```text
bin/logger_daemon
bin/logger_test
bin/syncchat_server
bin/syncchat_client
```

The project is compiled with:

```text
-Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L
```

## Running SyncChat

Run the logger daemon first:

```bash
./bin/logger_daemon
```

Run the server in another terminal:

```bash
./bin/syncchat_server
```

Run one or more clients:

```bash
./bin/syncchat_client
```

Runtime data is stored in:

```text
logs/server.log     server activity/performance log
storage/            server-side shared files
downloads/          client-side downloaded files
```

## Project Structure

```text
SyncChat/
├── include/
│   ├── client/
│   │   ├── download_client.h
│   │   └── upload_client.h
│   ├── common/
│   │   ├── file_transfer.h
│   │   ├── network_io.h
│   │   ├── performance_timer.h
│   │   └── protocol.h
│   ├── logger/
│   │   ├── logger_client.h
│   │   └── logger_protocol.h
│   └── server/
│       ├── client_handler.h
│       ├── client_manager.h
│       ├── file_lock.h
│       ├── file_manager.h
│       └── server_config.h
├── src/
│   ├── client/
│   │   ├── download_client.c
│   │   ├── main.c
│   │   └── upload_client.c
│   ├── common/
│   │   ├── file_transfer.c
│   │   ├── network_io.c
│   │   ├── performance_timer.c
│   │   └── protocol.c
│   ├── logger/
│   │   ├── logger_client.c
│   │   ├── main.c
│   │   └── test_client.c
│   └── server/
│       ├── client_handler.c
│       ├── client_manager.c
│       ├── file_lock.c
│       ├── file_manager.c
│       └── main.c
├── tests/
│   ├── STEP10.md
│   ├── step7_8_probe.py
│   └── stress_syncchat.py
├── docs/
│   └── wireshark/
│       ├── 01_tcp_handshake.png
│       ├── 02_chat_frame.png
│       ├── 03_file_transfer.png
│       ├── 04_tcp_termination.png
│       └── syncchat-evidence.pcapng
├── bin/
├── downloads/
├── logs/
├── storage/
├── Makefile
├── README.md
└── .gitignore
```

## Verification and Testing

The implemented system has been exercised with functional, security, concurrency, resource, and packet-level tests.

### Functional testing

Public chat, private messaging, `/users`, `/files`, upload, download, duplicate-file handling, missing-file handling, and graceful `/quit` behavior have been tested with multiple clients.

Binary upload/download integrity is checked using `cmp` and SHA-256 hashes.

### Security and failure testing

`tests/step7_8_probe.py` directly exercises the network protocol with malformed and hostile requests. Testing includes traversal/path-like names, malformed upload metadata, interrupted uploads, symbolic-link entries, cleanup of incomplete temporary files, and continued server usability after rejected requests.

### Stress and concurrency testing

`tests/stress_syncchat.py` verifies concurrent communication and file access.

Example commands:

```bash
python3 tests/stress_syncchat.py chat --clients 10 --messages 20
python3 tests/stress_syncchat.py chat --clients 25 --messages 20
python3 tests/stress_syncchat.py chat --clients 50 --messages 10 --wait 30

python3 tests/stress_syncchat.py download \
    --clients 10 \
    --file perf-test.bin \
    --expected /tmp/perf-test.bin

python3 tests/stress_syncchat.py limit --limit 64
```

Observed local stress runs successfully delivered all expected broadcast messages exactly once, completed concurrent downloads with identical hashes, accepted 64 registered clients, and rejected the additional client cleanly.

These localhost results demonstrate implementation behavior under local concurrent load; they should not be interpreted as guaranteed LAN or Internet throughput.

### Valgrind

The final server, CLI client, and logger daemon were tested with Valgrind Memcheck using full leak checking and file-descriptor tracking.

Final tested results:

```text
Server:
  in use at exit: 0 bytes in 0 blocks
  ERROR SUMMARY: 0 errors

CLI client:
  in use at exit: 0 bytes in 0 blocks
  ERROR SUMMARY: 0 errors

Logger daemon:
  in use at exit: 0 bytes in 0 blocks
  ERROR SUMMARY: 0 errors
```

Only the three inherited standard descriptors remained open at normal process exit.

### Wireshark

Packet evidence is stored in `docs/wireshark/`. The capture demonstrates:

```text
TCP three-way handshake
SyncChat framed chat traffic
DOWNLOAD_READY plus raw file-transfer traffic
graceful TCP connection termination
```

The capture file is preserved as `syncchat-evidence.pcapng` alongside screenshots.

## Current Limitations

The current implementation intentionally remains an educational systems-programming project rather than a production chat platform.

The present limitations include:

- No password-based authentication or account database; login is username registration for the active session.
- No transport encryption/TLS; application data is visible to packet capture.
- The server uses a thread-per-client model and a fixed 64-client registry rather than an event-driven architecture intended for thousands of connections.
- Shared files are limited to 50 MiB.
- The current CLI build is primarily configured for local testing; deployment configuration can be extended for arbitrary remote server addresses.
- File locking is advisory and Linux-specific open-file-description locking is used.
- The current user interface is command-line based. A Linux GTK client can be added as a presentation layer while preserving the same protocol and backend modules.
- Performance measurements are application-level observations from the tested environment and are not raw network-link bandwidth guarantees.

## Report / Implementation Consistency Notes

The final report should describe the implementation that actually exists.

The logging design is a custom Unix-domain datagram logger daemon. Server threads do **not** directly write a shared log file under one log mutex.

The implemented concurrency model is a main accept loop plus one detached POSIX thread per connected client. Do not claim that `select()` or `epoll()` is the active client-handling architecture unless such code is later added.

File locking is implemented with Linux `fcntl()` open-file-description locks. Downloads use shared locks and uploads use exclusive locks.

Duplicate shared filenames are rejected rather than automatically renamed.

The implemented performance evidence currently includes file-transfer elapsed time/application throughput and stress-test request/delivery rates. Do not claim that detailed CPU-utilization benchmarking has been completed unless separate CPU measurements are added.

Username login is not password authentication. Use terms such as **username registration**, **login**, or **session identity** rather than implying credential-based authentication.

Wireshark packet captures validate the TCP/application protocol on the tested interface. They are protocol evidence, not a substitute for real-LAN performance measurements.

## Presentation Interface

The communication layer is intentionally independent of the user interface. The CLI client remains useful for protocol testing, debugging, stress testing, and demonstrations of low-level behavior.

A Linux GTK presentation client can reuse the same protocol and backend operations for:

```text
username/login
chat history
send message
online-users panel
private messaging
shared-files panel
upload/download controls
connection/status display
logout
```

A GTK receiver thread must post UI updates to the GTK main thread rather than directly modifying widgets from a POSIX worker thread.

## Academic Focus

SyncChat demonstrates practical use of:

- Berkeley/POSIX TCP sockets
- network byte order
- application framing
- POSIX threads
- mutex synchronization
- condition variables
- Unix-domain socket IPC
- low-level file descriptors
- directory-relative file operations
- advisory file locking
- atomic filesystem operations
- monotonic timing
- signal handling
- defensive error handling
- Valgrind
- Wireshark
- concurrent stress testing

# License

This project was developed as a **Project of Network and Systems Programming** for academic purposes.

---

# Acknowledgements

The project team would like to express sincere gratitude to the project supervisor, project coordinator, department leadership, and everyone who provided guidance, feedback, and support throughout the development and documentation of SyncChat.

## Author

**Bipin Lamsal**

GitHub: [@beepennn](https://github.com/beepennn)
