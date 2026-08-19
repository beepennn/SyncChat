#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>

#include "common/network_io.h"


int send_all(
    int socket_fd,
    const void *buffer,
    size_t length
)
{
    const unsigned char *bytes =
        (const unsigned char *)buffer;

    size_t total_sent = 0;

    while (total_sent < length)
    {
        int send_flags = 0;

#ifdef MSG_NOSIGNAL
        send_flags |= MSG_NOSIGNAL;
#endif

        ssize_t sent = send(
            socket_fd,
            bytes + total_sent,
            length - total_sent,
            send_flags
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

        total_sent +=
            (size_t)sent;
    }

    return 0;
}


int recv_all(
    int socket_fd,
    void *buffer,
    size_t length
)
{
    unsigned char *bytes =
        (unsigned char *)buffer;

    size_t total_received = 0;

    while (total_received < length)
    {
        ssize_t received = recv(
            socket_fd,
            bytes + total_received,
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
            return 1;
        }

        total_received +=
            (size_t)received;
    }

    return 0;
}
