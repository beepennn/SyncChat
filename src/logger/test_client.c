#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "logger/logger_protocol.h"

int main(void)
{
    int socket_fd;
    struct sockaddr_un server_address;
    log_packet_t packet;

    socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);

    if (socket_fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_address, 0, sizeof(server_address));

    server_address.sun_family = AF_UNIX;

    strncpy(
        server_address.sun_path,
        LOGGER_SOCKET_PATH,
        sizeof(server_address.sun_path) - 1
    );

    memset(&packet, 0, sizeof(packet));

    packet.magic = LOGGER_MAGIC;
    packet.version = LOGGER_VERSION;
    packet.level = LOG_INFO;
    packet.sender_pid = getpid();

    strncpy(
        packet.source,
        "TEST_CLIENT",
        LOGGER_SOURCE_MAX - 1
    );

    strncpy(
        packet.message,
        "Logger IPC test message received successfully.",
        LOGGER_MESSAGE_MAX - 1
    );

    ssize_t sent = sendto(
        socket_fd,
        &packet,
        sizeof(packet),
        0,
        (struct sockaddr *)&server_address,
        sizeof(server_address)
    );

    if (sent < 0)
    {
        perror("sendto");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    if ((size_t)sent != sizeof(packet))
    {
        fprintf(stderr, "Incomplete log packet transmission.\n");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    printf("Log message sent successfully.\n");

    close(socket_fd);

    return EXIT_SUCCESS;
}
