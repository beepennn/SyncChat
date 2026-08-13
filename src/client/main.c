#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/network_io.h"
#include "common/protocol.h"
#include "server/server_config.h"


static int client_socket = -1;


/*
 * Send one length-prefixed application message.
 */
static int send_message(
    uint32_t type,
    const char *payload
)
{
    message_header_t header;
    size_t payload_length;

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
    header.length = htonl(
        (uint32_t)payload_length
    );

    if (send_all(
            client_socket,
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

    return send_all(
        client_socket,
        payload,
        payload_length
    );
}


/*
 * Receive one length-prefixed application message.
 *
 * Return:
 *   0  success
 *   1  peer disconnected
 *  -1  error
 */
static int receive_message(
    uint32_t *message_type,
    char *payload,
    size_t payload_size
)
{
    message_header_t header;

    uint32_t type;
    uint32_t length;

    int result = recv_all(
        client_socket,
        &header,
        sizeof(header)
    );

    if (result != 0)
    {
        return result;
    }

    type = ntohl(header.type);
    length = ntohl(header.length);

    if (!is_valid_message_type(type))
    {
        fprintf(
            stderr,
            "Invalid message type received: %u\n",
            type
        );

        return -1;
    }

    if (length > MESSAGE_MAX_SIZE)
    {
        fprintf(
            stderr,
            "Received message is too large.\n"
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
            client_socket,
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


static int connect_to_server(void)
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
    server_address.sin_port =
        htons(SERVER_PORT);

    if (inet_pton(
            AF_INET,
            "127.0.0.1",
            &server_address.sin_addr
        ) != 1)
    {
        fprintf(
            stderr,
            "Invalid server address.\n"
        );

        close(client_socket);
        client_socket = -1;

        return -1;
    }

    if (connect(
            client_socket,
            (struct sockaddr *)&server_address,
            sizeof(server_address)
        ) < 0)
    {
        perror("connect");

        close(client_socket);
        client_socket = -1;

        return -1;
    }

    return 0;
}


static void cleanup(void)
{
    if (client_socket >= 0)
    {
        close(client_socket);
        client_socket = -1;
    }
}


int main(void)
{
    char payload[MESSAGE_MAX_SIZE + 1];

    uint32_t message_type;

    if (connect_to_server() != 0)
    {
        return EXIT_FAILURE;
    }

    printf(
        "Connected to SyncChat server.\n"
    );

    /*
     * Wait for USERNAME_REQUIRED.
     */
    int result = receive_message(
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
            "Failed to receive server login request.\n"
        );

        cleanup();

        return EXIT_FAILURE;
    }

    if (message_type != MSG_RESPONSE ||
        strcmp(
            payload,
            "USERNAME_REQUIRED"
        ) != 0)
    {
        fprintf(
            stderr,
            "Unexpected server response: %s\n",
            payload
        );

        cleanup();

        return EXIT_FAILURE;
    }

    /*
     * Ask for username.
     */
    char username[USERNAME_MAX_SIZE];

    while (1)
    {
        printf("Username: ");
        fflush(stdout);

        if (fgets(
                username,
                sizeof(username),
                stdin
            ) == NULL)
        {
            fprintf(
                stderr,
                "Failed to read username.\n"
            );

            cleanup();

            return EXIT_FAILURE;
        }

        username[
            strcspn(username, "\n")
        ] = '\0';

        if (!is_valid_username(username))
        {
            printf(
                "Invalid username. "
                "Use letters, digits, and underscore only.\n"
            );

            continue;
        }

        break;
    }

    if (send_message(
            MSG_LOGIN,
            username
        ) != 0)
    {
        perror("send login");

        cleanup();

        return EXIT_FAILURE;
    }

    /*
     * Receive login result.
     */
    result = receive_message(
        &message_type,
        payload,
        sizeof(payload)
    );

    if (result == 1)
    {
        fprintf(
            stderr,
            "Server disconnected during login.\n"
        );

        cleanup();

        return EXIT_FAILURE;
    }

    if (result < 0)
    {
        fprintf(
            stderr,
            "Failed to receive login response.\n"
        );

        cleanup();

        return EXIT_FAILURE;
    }

    if (message_type == MSG_ERROR)
    {
        printf(
            "Login rejected: %s\n",
            payload
        );

        cleanup();

        return EXIT_FAILURE;
    }

    if (message_type != MSG_RESPONSE ||
        strncmp(
            payload,
            "LOGIN_SUCCESS",
            strlen("LOGIN_SUCCESS")
        ) != 0)
    {
        fprintf(
            stderr,
            "Unexpected login response: %s\n",
            payload
        );

        cleanup();

        return EXIT_FAILURE;
    }

    printf(
        "Login successful as '%s'.\n",
        username
    );

    /*
     * Persistent chat loop.
     */
    while (1)
    {
        printf(
            "%s> ",
            username
        );

        fflush(stdout);

        if (fgets(
                payload,
                sizeof(payload),
                stdin
            ) == NULL)
        {
            break;
        }

        payload[
            strcspn(payload, "\n")
        ] = '\0';

        if (payload[0] == '\0')
        {
            continue;
        }

        /*
         * Graceful disconnect handshake.
         */
        if (strcmp(
                payload,
                "/quit"
            ) == 0)
        {
            if (send_message(
                    MSG_DISCONNECT,
                    ""
                ) != 0)
            {
                fprintf(
                    stderr,
                    "Failed to send disconnect request.\n"
                );

                break;
            }

            /*
             * Wait for the server's GOODBYE response
             * before closing the socket.
             */
            result = receive_message(
                &message_type,
                payload,
                sizeof(payload)
            );

            if (result == 0 &&
                message_type == MSG_RESPONSE &&
                strcmp(
                    payload,
                    "GOODBYE"
                ) == 0)
            {
                printf(
                    "Server: GOODBYE\n"
                );
            }
            else if (result == 1)
            {
                printf(
                    "Server closed the connection.\n"
                );
            }
            else if (result < 0)
            {
                fprintf(
                    stderr,
                    "Failed to receive disconnect response.\n"
                );
            }

            break;
        }

        if (send_message(
                MSG_CHAT,
                payload
            ) != 0)
        {
            fprintf(
                stderr,
                "Failed to send message.\n"
            );

            break;
        }

        /*
         * Wait for either:
         *
         *   MSG_RESPONSE
         *   MSG_BROADCAST
         *   MSG_ERROR
         */
        result = receive_message(
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

            break;
        }

        if (result < 0)
        {
            fprintf(
                stderr,
                "Failed to receive server response.\n"
            );

            break;
        }

        if (message_type == MSG_ERROR)
        {
            printf(
                "Server error: %s\n",
                payload
            );
        }
        else if (message_type == MSG_BROADCAST)
        {
            printf(
                "%s\n",
                payload
            );
        }
        else
        {
            printf(
                "Server: %s\n",
                payload
            );
        }
    }

    cleanup();

    printf(
        "Disconnected from server.\n"
    );

    return EXIT_SUCCESS;
}
