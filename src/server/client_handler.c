#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/network_io.h"
#include "common/protocol.h"
#include "server/client_handler.h"
#include "server/client_manager.h"


/*
 * Send one length-prefixed application message.
 *
 * Wire format:
 *
 * +----------------------+----------------------+
 * | Type (4 bytes)       | Length (4 bytes)     |
 * +----------------------+----------------------+
 * | Payload (N bytes)                           |
 * +---------------------------------------------+
 */
static int send_message(
    int socket_fd,
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

    return send_all(
        socket_fd,
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

    if (!is_valid_message_type(type))
    {
        fprintf(
            stderr,
            "Invalid message type: %u\n",
            type
        );

        return -1;
    }

    if (length > MESSAGE_MAX_SIZE)
    {
        fprintf(
            stderr,
            "Message exceeds maximum size: %u\n",
            length
        );

        return -1;
    }

    /*
     * One extra byte is required for '\0'.
     */
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


/*
 * Receive and validate the initial login request.
 *
 * Return:
 *   0  valid login packet
 *   1  peer disconnected
 *   2  invalid username
 *  -1  protocol/communication error
 */
static int perform_login(
    int socket_fd,
    char *username,
    size_t username_size
)
{
    uint32_t message_type;

    int result = receive_message(
        socket_fd,
        &message_type,
        username,
        username_size
    );

    if (result == 1)
    {
        return 1;
    }

    if (result < 0)
    {
        return -1;
    }

    if (message_type != MSG_LOGIN)
    {
        fprintf(
            stderr,
            "Expected MSG_LOGIN, received type %u.\n",
            message_type
        );

        return -1;
    }

    if (!is_valid_username(username))
    {
        return 2;
    }

    return 0;
}


/*
 * Handle a single connected client.
 *
 * Ownership of socket_fd is transferred to this thread.
 */
void *client_handler(void *argument)
{
    client_context_t *context = argument;

    if (context == NULL)
    {
        return NULL;
    }

    /*
     * Copy the socket descriptor before releasing
     * the dynamically allocated context.
     */
    int socket_fd = context->socket_fd;

    free(context);
    context = NULL;

    pthread_t thread_id = pthread_self();

    int client_id = -1;
    int remove_result = -1;

    printf(
        "Client thread [%lu] started.\n",
        (unsigned long)thread_id
    );

    /*
     * Ask the client for a username.
     */
    if (send_message(
            socket_fd,
            MSG_RESPONSE,
            "USERNAME_REQUIRED"
        ) != 0)
    {
        fprintf(
            stderr,
            "Thread [%lu]: failed to send login request.\n",
            (unsigned long)thread_id
        );

        close(socket_fd);

        return NULL;
    }

    /*
     * Receive username.
     */
    char username[USERNAME_MAX_SIZE];

    memset(
        username,
        0,
        sizeof(username)
    );

    int login_result = perform_login(
        socket_fd,
        username,
        sizeof(username)
    );

    if (login_result == 1)
    {
        printf(
            "Thread [%lu]: client disconnected during login.\n",
            (unsigned long)thread_id
        );

        close(socket_fd);

        return NULL;
    }

    if (login_result == 2)
    {
        if (send_message(
                socket_fd,
                MSG_ERROR,
                "INVALID_USERNAME"
            ) != 0)
        {
            fprintf(
                stderr,
                "Thread [%lu]: failed to send invalid-username response.\n",
                (unsigned long)thread_id
            );
        }

        close(socket_fd);

        printf(
            "Thread [%lu]: invalid username rejected.\n",
            (unsigned long)thread_id
        );

        return NULL;
    }

    if (login_result != 0)
    {
        if (send_message(
                socket_fd,
                MSG_ERROR,
                "LOGIN_FAILED"
            ) != 0)
        {
            fprintf(
                stderr,
                "Thread [%lu]: failed to send login failure response.\n",
                (unsigned long)thread_id
            );
        }

        close(socket_fd);

        printf(
            "Thread [%lu]: login failed.\n",
            (unsigned long)thread_id
        );

        return NULL;
    }

    /*
     * Register the client.
     *
     * Username uniqueness is checked and insertion is
     * performed inside the same mutex-protected operation
     * in client_manager_add().
     */
    client_id = client_manager_add(
        socket_fd,
        thread_id,
        username
    );

    /*
     * -2 means duplicate username.
     */
    if (client_id == -2)
    {
        if (send_message(
                socket_fd,
                MSG_ERROR,
                "USERNAME_ALREADY_EXISTS"
            ) != 0)
        {
            fprintf(
                stderr,
                "Thread [%lu]: failed to send duplicate-username response.\n",
                (unsigned long)thread_id
            );
        }

        close(socket_fd);

        printf(
            "Thread [%lu]: username '%s' already exists.\n",
            (unsigned long)thread_id,
            username
        );

        return NULL;
    }

    /*
     * Any other negative result indicates a registry
     * or internal failure.
     */
    if (client_id < 0)
    {
        if (send_message(
                socket_fd,
                MSG_ERROR,
                "SERVER_CLIENT_LIMIT"
            ) != 0)
        {
            fprintf(
                stderr,
                "Thread [%lu]: failed to send client-limit response.\n",
                (unsigned long)thread_id
            );
        }

        close(socket_fd);

        printf(
            "Thread [%lu]: client registry full or unavailable.\n",
            (unsigned long)thread_id
        );

        return NULL;
    }

    printf(
        "Client %d registered: %s\n",
        client_id,
        username
    );

    /*
     * Confirm successful login.
     */
    char login_message[MESSAGE_MAX_SIZE + 1];

    int login_message_length = snprintf(
        login_message,
        sizeof(login_message),
        "LOGIN_SUCCESS %s",
        username
    );

    if (login_message_length < 0 ||
        (size_t)login_message_length >=
            sizeof(login_message))
    {
        fprintf(
            stderr,
            "Client %d: failed to construct login response.\n",
            client_id
        );

        client_manager_remove(socket_fd);
        close(socket_fd);

        return NULL;
    }

    if (send_message(
            socket_fd,
            MSG_RESPONSE,
            login_message
        ) != 0)
    {
        fprintf(
            stderr,
            "Client %d: failed to send login confirmation.\n",
            client_id
        );

        client_manager_remove(socket_fd);
        close(socket_fd);

        return NULL;
    }

    /*
     * Persistent communication loop.
     */
    char payload[MESSAGE_MAX_SIZE + 1];

    while (1)
    {
        uint32_t message_type;

        int result = receive_message(
            socket_fd,
            &message_type,
            payload,
            sizeof(payload)
        );

        /*
         * Client closed the TCP connection.
         */
        if (result == 1)
        {
            printf(
                "Client %d (%s) disconnected.\n",
                client_id,
                username
            );

            break;
        }

        /*
         * Protocol or communication error.
         */
        if (result < 0)
        {
            fprintf(
                stderr,
                "Client %d (%s): receive error.\n",
                client_id,
                username
            );

            break;
        }

        switch (message_type)
        {
            case MSG_CHAT:
            {
                char broadcast_message[
                    MESSAGE_MAX_SIZE +
                    USERNAME_MAX_SIZE +
                    8
                ];

                int broadcast_length = snprintf(
                    broadcast_message,
                    sizeof(broadcast_message),
                    "%s: %s",
                    username,
                    payload
                );

                if (broadcast_length < 0 ||
                    (size_t)broadcast_length >=
                        sizeof(broadcast_message))
                {
                    fprintf(
                        stderr,
                        "Client %d: failed to format broadcast message.\n",
                        client_id
                    );

                    if (send_message(
                            socket_fd,
                            MSG_ERROR,
                            "MESSAGE_TOO_LARGE"
                        ) != 0)
                    {
                        goto connection_end;
                    }

                    break;
                }

                printf(
                    "Broadcast from Client %d (%s): %s\n",
                    client_id,
                    username,
                    payload
                );

                /*
                 * client_manager_broadcast():
                 *
                 * 1. Locks the registry.
                 * 2. Takes references to target sockets.
                 * 3. Copies target socket descriptors.
                 * 4. Unlocks the registry.
                 * 5. Performs send() operations without
                 *    holding the global registry mutex.
                 */
                int recipients =
                    client_manager_broadcast(
                        socket_fd,
                        MSG_BROADCAST,
                        broadcast_message
                    );

                if (recipients < 0)
                {
                    fprintf(
                        stderr,
                        "Client %d: broadcast failed.\n",
                        client_id
                    );

                    if (send_message(
                            socket_fd,
                            MSG_ERROR,
                            "BROADCAST_FAILED"
                        ) != 0)
                    {
                        goto connection_end;
                    }

                    break;
                }

                /*
                 * Acknowledge the sender.
                 */
                char acknowledgement[128];

                int acknowledgement_length =
                    snprintf(
                        acknowledgement,
                        sizeof(acknowledgement),
                        "MESSAGE_DELIVERED %d",
                        recipients
                    );

                if (acknowledgement_length < 0 ||
                    (size_t)acknowledgement_length >=
                        sizeof(acknowledgement))
                {
                    goto connection_end;
                }

                if (send_message(
                        socket_fd,
                        MSG_RESPONSE,
                        acknowledgement
                    ) != 0)
                {
                    goto connection_end;
                }

                break;
            }


            case MSG_DISCONNECT:

                printf(
                    "Client %d (%s) requested disconnect.\n",
                    client_id,
                    username
                );

                if (send_message(
                        socket_fd,
                        MSG_RESPONSE,
                        "GOODBYE"
                    ) != 0)
                {
                    fprintf(
                        stderr,
                        "Client %d: failed to send GOODBYE response.\n",
                        client_id
                    );
                }

                goto connection_end;


            case MSG_UPLOAD:

                if (send_message(
                        socket_fd,
                        MSG_ERROR,
                        "FILE_UPLOAD_NOT_IMPLEMENTED"
                    ) != 0)
                {
                    goto connection_end;
                }

                break;


            case MSG_DOWNLOAD:

                if (send_message(
                        socket_fd,
                        MSG_ERROR,
                        "FILE_DOWNLOAD_NOT_IMPLEMENTED"
                    ) != 0)
                {
                    goto connection_end;
                }

                break;


            case MSG_LIST_FILES:

                if (send_message(
                        socket_fd,
                        MSG_ERROR,
                        "FILE_LIST_NOT_IMPLEMENTED"
                    ) != 0)
                {
                    goto connection_end;
                }

                break;


            default:

                if (send_message(
                        socket_fd,
                        MSG_ERROR,
                        "UNSUPPORTED_OPERATION"
                    ) != 0)
                {
                    goto connection_end;
                }

                break;
        }
    }


connection_end:

    /*
     * Remove the client from the registry before closing
     * the socket.
     *
     * client_manager_remove() waits for outstanding
     * broadcast references before allowing the socket
     * to be released.
     */
    remove_result =
        client_manager_remove(socket_fd);

    if (remove_result == 0)
    {
        printf(
            "Client %d (%s) removed from client manager.\n",
            client_id,
            username
        );
    }
    else if (remove_result == 1)
    {
        fprintf(
            stderr,
            "Client %d was not found in registry.\n",
            client_id
        );
    }
    else
    {
        fprintf(
            stderr,
            "Client %d: failed to update client registry.\n",
            client_id
        );
    }

    /*
     * Release the socket after registry cleanup.
     */
    if (close(socket_fd) != 0)
    {
        perror("close client socket");
    }

    printf(
        "Active clients: %zu\n",
        client_manager_count()
    );

    printf(
        "Client thread [%lu] terminated.\n",
        (unsigned long)thread_id
    );

    return NULL;
}
