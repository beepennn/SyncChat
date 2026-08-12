#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/network_io.h"
#include "common/protocol.h"
#include "server/server_config.h"

static int client_socket = -1;


/*
 * Release client resources.
 */
static void cleanup(void)
{
    if (client_socket >= 0)
    {
        close(client_socket);
        client_socket = -1;
    }
}


/*
 * Send one length-prefixed application message.
 */
static int send_message(
    int socket_fd,
    uint32_t type,
    const char *payload
)
{
    size_t payload_length;
    message_header_t header;

    if (payload == NULL)
    {
        return -1;
    }

    payload_length = strlen(payload);

    if (payload_length > MESSAGE_MAX_SIZE)
    {
        fprintf(
            stderr,
            "Message exceeds maximum size.\n"
        );

        return -1;
    }

    header.type = htonl(type);
    header.length = htonl((uint32_t)payload_length);

    if (send_all(
            socket_fd,
            &header,
            sizeof(header)
        ) != 0)
    {
        return -1;
    }

    if (payload_length == 0)
    {
        return 0;
    }

    if (send_all(
            socket_fd,
            payload,
            payload_length
        ) != 0)
    {
        return -1;
    }

    return 0;
}


/*
 * Receive one length-prefixed application message.
 */
static int receive_message(
    int socket_fd,
    uint32_t *message_type,
    char *payload,
    size_t payload_size
)
{
    message_header_t header;

    uint32_t type;
    uint32_t length;

    int result;

    if (message_type == NULL ||
        payload == NULL ||
        payload_size == 0)
    {
        return -1;
    }

    result = recv_all(
        socket_fd,
        &header,
        sizeof(header)
    );

    if (result != 0)
    {
        return result;
    }

    type = ntohl(header.type);
    length = ntohl(header.length);

    if (length > MESSAGE_MAX_SIZE)
    {
        fprintf(
            stderr,
            "Received message exceeds maximum size: %u\n",
            length
        );

        return -1;
    }

    if ((size_t)length >= payload_size)
    {
        fprintf(
            stderr,
            "Payload buffer is too small.\n"
        );

        return -1;
    }

    if (length > 0)
    {
        result = recv_all(
            socket_fd,
            payload,
            length
        );

        if (result != 0)
        {
            return result;
        }
    }

    payload[length] = '\0';

    *message_type = type;

    return 0;
}


static int connect_to_server(
    const char *server_ip,
    uint16_t port
)
{
    struct sockaddr_in server_address;

    client_socket = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (client_socket < 0)
    {
        perror("socket");
        return -1;
    }

    memset(
        &server_address,
        0,
        sizeof(server_address)
    );

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);

    if (inet_pton(
            AF_INET,
            server_ip,
            &server_address.sin_addr
        ) != 1)
    {
        fprintf(
            stderr,
            "Invalid server IP address: %s\n",
            server_ip
        );

        cleanup();

        return -1;
    }

    if (connect(
            client_socket,
            (struct sockaddr *)&server_address,
            sizeof(server_address)
        ) < 0)
    {
        perror("connect");

        cleanup();

        return -1;
    }

    return 0;
}


int main(void)
{
    char payload[MESSAGE_MAX_SIZE + 1];

    uint32_t message_type;

    int result;

    if (connect_to_server(
            "127.0.0.1",
            SERVER_PORT
        ) != 0)
    {
        return EXIT_FAILURE;
    }

    printf(
        "Connected to SyncChat server.\n"
    );

    /*
     * Receive the server greeting.
     */
    result = receive_message(
        client_socket,
        &message_type,
        payload,
        sizeof(payload)
    );

    if (result == 1)
    {
        fprintf(
            stderr,
            "Server disconnected.\n"
        );

        cleanup();

        return EXIT_FAILURE;
    }

    if (result < 0)
    {
        fprintf(
            stderr,
            "Failed to receive server greeting.\n"
        );

        cleanup();

        return EXIT_FAILURE;
    }

    printf(
        "Server: %s\n",
        payload
    );

    /*
     * Basic single-message test.
     */
    printf(
        "Enter a message: "
    );

    fflush(stdout);

    if (fgets(
            payload,
            sizeof(payload),
            stdin
        ) == NULL)
    {
        fprintf(
            stderr,
            "Failed to read input.\n"
        );

        cleanup();

        return EXIT_FAILURE;
    }

    payload[strcspn(payload, "\n")] = '\0';

    if (send_message(
            client_socket,
            MSG_CHAT,
            payload
        ) != 0)
    {
        perror("send_message");

        cleanup();

        return EXIT_FAILURE;
    }

    printf(
        "Message sent successfully.\n"
    );

    cleanup();

    return EXIT_SUCCESS;
}
