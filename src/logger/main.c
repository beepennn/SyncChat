#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "logger/logger_protocol.h"

static volatile sig_atomic_t running = 1;

static int logger_socket = -1;
static int log_file = -1;

/*
 * Forward declaration because cleanup() is used
 * during signal-handler setup failure paths.
 */
static void cleanup(void);


/*
 * Handle SIGINT and SIGTERM.

 * Only modify sig_atomic_t state inside
 * the signal handler. Resource cleanup is
 * performed by the normal execution flow.
 */
static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}


/*
 * Convert a log level to its textual representation.
 */
static const char *log_level_name(log_level_t level)
{
    switch (level)
    {
        case LOG_DEBUG:
            return "DEBUG";

        case LOG_INFO:
            return "INFO";

        case LOG_WARN:
            return "WARN";

        case LOG_ERROR:
            return "ERROR";

        default:
            return "UNKNOWN";
    }
}


/*
 * Write the complete buffer to a file descriptor.
 *
 * write() may write fewer bytes than requested,
 * so this function continues until all data has
 * been written.
 */
static int write_all(int fd, const void *buffer, size_t length)
{
    const char *data = buffer;
    size_t total_written = 0;

    while (total_written < length)
    {
        ssize_t written = write(
            fd,
            data + total_written,
            length - total_written
        );

        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        if (written == 0)
        {
            return -1;
        }

        total_written += (size_t)written;
    }

    return 0;
}


/*
 * Validate the received logging packet.
 *
 * The packet is read-only during validation.
 */
static int validate_packet(const log_packet_t *packet)
{
    size_t source_length;
    size_t message_length;

    if (packet->magic != LOGGER_MAGIC)
    {
        return -1;
    }

    if (packet->version != LOGGER_VERSION)
    {
        return -1;
    }

    if (packet->level > LOG_ERROR)
    {
        return -1;
    }

    source_length = strnlen(
        packet->source,
        LOGGER_SOURCE_MAX
    );

    if (source_length == LOGGER_SOURCE_MAX)
    {
        return -1;
    }

    message_length = strnlen(
        packet->message,
        LOGGER_MESSAGE_MAX
    );

    if (message_length == LOGGER_MESSAGE_MAX)
    {
        return -1;
    }

    if (source_length == 0)
    {
        return -1;
    }

    if (message_length == 0)
    {
        return -1;
    }

    return 0;
}


/*
 * Generate a local calendar timestamp.
 *
 * clock_gettime() obtains the current system time.
 * The timestamp formatting itself is performed by
 * this application.
 */
static int create_timestamp(char *buffer, size_t buffer_size)
{
    struct timespec current_time;
    struct tm local_time;

    if (clock_gettime(CLOCK_REALTIME, &current_time) != 0)
    {
        return -1;
    }

    if (localtime_r(
            &current_time.tv_sec,
            &local_time
        ) == NULL)
    {
        return -1;
    }

    int length = snprintf(
        buffer,
        buffer_size,
        "%04d-%02d-%02d %02d:%02d:%02d",
        local_time.tm_year + 1900,
        local_time.tm_mon + 1,
        local_time.tm_mday,
        local_time.tm_hour,
        local_time.tm_min,
        local_time.tm_sec
    );

    if (length < 0)
    {
        return -1;
    }

    if ((size_t)length >= buffer_size)
    {
        return -1;
    }

    return 0;
}


/*
 * Format and write a complete log entry.
 */
static int write_log_entry(const log_packet_t *packet)
{
    char timestamp[32];
    char entry[LOGGER_MESSAGE_MAX + 128];

    if (create_timestamp(
            timestamp,
            sizeof(timestamp)
        ) != 0)
    {
        return -1;
    }

    int length = snprintf(
        entry,
        sizeof(entry),
        "[%s] [PID:%ld] [%s] [%s] %s\n",
        timestamp,
        (long)packet->sender_pid,
        log_level_name((log_level_t)packet->level),
        packet->source,
        packet->message
    );

    if (length < 0)
    {
        return -1;
    }

    if ((size_t)length >= sizeof(entry))
    {
        return -1;
    }

    if (write_all(
            log_file,
            entry,
            (size_t)length
        ) != 0)
    {
        return -1;
    }

    /*
     * Display the same formatted entry on the
     * daemon console.
     */
    if (fputs(entry, stdout) == EOF)
    {
        /*
         * Console output failure should not corrupt
         * the already-written log file. The log write
         * has already succeeded.
         */
        clearerr(stdout);
    }

    return 0;
}


/*
 * Create and bind the Unix-domain datagram socket.
 */
static int create_logger_socket(void)
{
    struct sockaddr_un address;

    logger_socket = socket(
        AF_UNIX,
        SOCK_DGRAM,
        0
    );

    if (logger_socket < 0)
    {
        perror("socket");
        return -1;
    }

    memset(&address, 0, sizeof(address));

    address.sun_family = AF_UNIX;

    if (snprintf(
            address.sun_path,
            sizeof(address.sun_path),
            "%s",
            LOGGER_SOCKET_PATH
        ) >= (int)sizeof(address.sun_path))
    {
        fprintf(
            stderr,
            "Logger socket path is too long.\n"
        );

        close(logger_socket);
        logger_socket = -1;

        return -1;
    }

    /*
     * Remove any stale socket pathname left by
     * a previous abnormal termination.
     */
    if (unlink(LOGGER_SOCKET_PATH) != 0)
    {
        if (errno != ENOENT)
        {
            perror("unlink stale socket");

            close(logger_socket);
            logger_socket = -1;

            return -1;
        }
    }

    if (bind(
            logger_socket,
            (struct sockaddr *)&address,
            sizeof(address)
        ) < 0)
    {
        perror("bind");

        close(logger_socket);
        logger_socket = -1;

        return -1;
    }

    return 0;
}


/*
 * Open the server log file using low-level file I/O.
 */
static int open_log_file(void)
{
    log_file = open(
        LOGGER_LOG_FILE,
        O_WRONLY | O_CREAT | O_APPEND,
        0644
    );

    if (log_file < 0)
    {
        perror("open log file");
        return -1;
    }

    return 0;
}


/*
 * Release all logger resources.
 */
static void cleanup(void)
{
    if (logger_socket >= 0)
    {
        if (close(logger_socket) != 0)
        {
            perror("close logger socket");
        }

        logger_socket = -1;
    }

    if (log_file >= 0)
    {
        if (close(log_file) != 0)
        {
            perror("close log file");
        }

        log_file = -1;
    }

    /*
     * Remove the Unix-domain socket pathname.
     */
    if (unlink(LOGGER_SOCKET_PATH) != 0)
    {
        if (errno != ENOENT)
        {
            perror("unlink logger socket");
        }
    }
}


int main(void)
{
    struct sockaddr_un client_address;
    socklen_t client_address_length;

    log_packet_t packet;

    /*
     * Configure POSIX signal handling.
     */
    struct sigaction action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = handle_signal;

    if (sigemptyset(&action.sa_mask) != 0)
    {
        perror("sigemptyset");

        return EXIT_FAILURE;
    }

    /*
     * Do not use SA_RESTART here.
     *
     * We want SIGINT/SIGTERM to interrupt recvfrom()
     * so that the main loop can observe running == 0
     * and perform normal cleanup.
     */
    action.sa_flags = 0;

    if (sigaction(
            SIGINT,
            &action,
            NULL
        ) != 0)
    {
        perror("sigaction SIGINT");

        return EXIT_FAILURE;
    }

    if (sigaction(
            SIGTERM,
            &action,
            NULL
        ) != 0)
    {
        perror("sigaction SIGTERM");

        return EXIT_FAILURE;
    }

    /*
     * Create the logging IPC socket.
     */
    if (create_logger_socket() != 0)
    {
        return EXIT_FAILURE;
    }

    /*
     * Open the persistent log file.
     */
    if (open_log_file() != 0)
    {
        cleanup();

        return EXIT_FAILURE;
    }

    printf("Custom logger daemon started.\n");
    printf("Socket: %s\n", LOGGER_SOCKET_PATH);
    printf("Log file: %s\n", LOGGER_LOG_FILE);

    /*
     * Main logging loop.
     */
    while (running)
    {
        memset(
            &packet,
            0,
            sizeof(packet)
        );

        memset(
            &client_address,
            0,
            sizeof(client_address)
        );

        client_address_length =
            sizeof(client_address);

        ssize_t received = recvfrom(
            logger_socket,
            &packet,
            sizeof(packet),
            0,
            (struct sockaddr *)&client_address,
            &client_address_length
        );

        if (received < 0)
        {
            /*
             * SIGINT/SIGTERM will normally interrupt
             * recvfrom() with EINTR.
             */
            if (errno == EINTR)
            {
                continue;
            }

            perror("recvfrom");

            break;
        }

        /*
         * SOCK_DGRAM preserves message boundaries.
         * A valid message must therefore contain the
         * complete packet.
         */
        if ((size_t)received != sizeof(log_packet_t))
        {
            fprintf(
                stderr,
                "Rejected malformed log packet: %zd bytes\n",
                received
            );

            continue;
        }

        /*
         * Validate protocol fields before using them.
         */
        if (validate_packet(&packet) != 0)
        {
            fprintf(
                stderr,
                "Rejected invalid log packet.\n"
            );

            continue;
        }

        /*
         * Format and persist the accepted log entry.
         */
        if (write_log_entry(&packet) != 0)
        {
            perror("write log entry");

            break;
        }
    }

    /*
     * Normal cleanup is performed outside the signal
     * handler.
     */
    cleanup();

    printf("Custom logger daemon stopped.\n");

    return EXIT_SUCCESS;
}
