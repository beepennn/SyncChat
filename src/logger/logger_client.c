#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "logger/logger_client.h"
#include "logger/logger_protocol.h"


int logger_client_log(
    log_level_t level,
    const char *source,
    const char *format,
    ...
)
{
    if (source == NULL ||
        format == NULL)
    {
        return -1;
    }

    if (level < LOG_DEBUG ||
        level > LOG_ERROR)
    {
        return -1;
    }

    /*
     * Do not allow an unterminated/oversized
     * source name.
     */
    size_t source_length =
        strlen(source);

    if (source_length == 0 ||
        source_length >= LOGGER_SOURCE_MAX)
    {
        return -1;
    }


    log_packet_t packet;

    memset(
        &packet,
        0,
        sizeof(packet)
    );


    packet.magic =
        LOGGER_MAGIC;

    packet.version =
        LOGGER_VERSION;

    packet.level =
        (uint16_t)level;

    packet.sender_pid =
        getpid();


    /*
     * Source length has already been validated.
     */
    memcpy(
        packet.source,
        source,
        source_length
    );

    packet.source[source_length] =
        '\0';


    /*
     * Format log message into the packet.
     */
    va_list arguments;

    va_start(
        arguments,
        format
    );

    int written = vsnprintf(
        packet.message,
        sizeof(packet.message),
        format,
        arguments
    );

    va_end(arguments);


    if (written < 0)
    {
        return -1;
    }

    /*
     * vsnprintf guarantees termination when the
     * destination size is greater than zero.
     *
     * Messages exceeding LOGGER_MESSAGE_MAX are
     * safely truncated rather than affecting the
     * main server operation.
     */
    packet.message[
        LOGGER_MESSAGE_MAX - 1
    ] = '\0';


    /*
     * Use a short-lived AF_UNIX datagram socket.
     *
     * This avoids maintaining shared logger state
     * between multiple detached client threads.
     */
    int logger_socket = socket(
        AF_UNIX,
        SOCK_DGRAM,
        0
    );

    if (logger_socket < 0)
    {
        return -1;
    }


    struct sockaddr_un logger_address;

    memset(
        &logger_address,
        0,
        sizeof(logger_address)
    );

    logger_address.sun_family =
        AF_UNIX;


    size_t socket_path_length =
        strlen(LOGGER_SOCKET_PATH);

    if (socket_path_length >=
        sizeof(logger_address.sun_path))
    {
        close(logger_socket);

        return -1;
    }


    memcpy(
        logger_address.sun_path,
        LOGGER_SOCKET_PATH,
        socket_path_length + 1
    );


    ssize_t sent = sendto(
        logger_socket,
        &packet,
        sizeof(packet),
        0,
        (struct sockaddr *)&logger_address,
        sizeof(logger_address)
    );


    int close_result =
        close(logger_socket);


    if (sent !=
        (ssize_t)sizeof(packet))
    {
        return -1;
    }


    if (close_result != 0)
    {
        return -1;
    }


    return 0;
}
