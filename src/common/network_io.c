#include <errno.h>
#include <sys/socket.h>

#include "common/network_io.h"

int send_all(int socket_fd, const void *buffer, size_t length)
{
    const char *data = buffer;
    size_t total_sent = 0;

    while (total_sent < length)
    {
        ssize_t sent = send(
            socket_fd,
            data + total_sent,
            length - total_sent,
            0
        );

        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        if (sent == 0)
        {
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}


int recv_all(int socket_fd, void *buffer, size_t length)
{
    char *data = buffer;
    size_t total_received = 0;

    while (total_received < length)
    {
        ssize_t received = recv(
            socket_fd,
            data + total_received,
            length - total_received,
            0
        );

        if (received < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        if (received == 0)
        {
            /*
             * Peer closed the connection before the
             * expected amount of data was received.
             */
            return 1;
        }

        total_received += (size_t)received;
    }

    return 0;
}
