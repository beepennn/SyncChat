#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/network_io.h"
#include "common/protocol.h"
#include "server/client_handler.h"
#include "server/client_manager.h"


/*
 * Raw framed send.
 *
 * This function is used only during the login phase,
 * before the client has been registered in the shared
 * client manager.
 *
 * After successful registration, all outgoing server
 * messages must use client_manager_send() so that
 * writes to each TCP stream are serialized.
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

    if (!is_valid_message_type(type))
    {
        return -1;
    }

    payload_length = strlen(payload);

    if (payload_length > MESSAGE_MAX_SIZE)
    {
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
 * Receive one complete framed application message.
 *
 * Return values:
 *
 *   0  complete message received
 *   1  peer closed the connection
 *  -1  protocol/socket error
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
            "Client sent invalid message type: %u\n",
            type
        );

        return -1;
    }

    if (length > MESSAGE_MAX_SIZE)
    {
        fprintf(
            stderr,
            "Client sent oversized payload: %u bytes\n",
            length
        );

        return -1;
    }

    if ((size_t)length >= payload_size)
    {
        fprintf(
            stderr,
            "Client payload buffer is too small.\n"
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
 * Perform the initial username/login handshake.
 *
 * Login-phase messages use send_message() because the
 * client has not yet been entered into the registry.
 *
 * Once client_manager_add() succeeds, every subsequent
 * server write goes through client_manager_send().
 *
 * Return:
 *
 *   >0  assigned client ID
 *    0  login rejected
 *   -1  connection/internal error
 */
static int perform_login(
    int socket_fd,
    char *username,
    size_t username_size
)
{
    char payload[MESSAGE_MAX_SIZE + 1];

    uint32_t message_type;

    int result;

    if (username == NULL ||
        username_size < USERNAME_MAX_SIZE)
    {
        return -1;
    }

    /*
     * Ask the newly connected client for a username.
     */
    if (send_message(
            socket_fd,
            MSG_RESPONSE,
            "USERNAME_REQUIRED"
        ) != 0)
    {
        fprintf(
            stderr,
            "Failed to request username from client.\n"
        );

        return -1;
    }

    /*
     * Receive LOGIN request.
     */
    result = receive_message(
        socket_fd,
        &message_type,
        payload,
        sizeof(payload)
    );

    if (result == 1)
    {
        return -1;
    }

    if (result < 0)
    {
        fprintf(
            stderr,
            "Failed to receive login request.\n"
        );

        return -1;
    }

    if (message_type != MSG_LOGIN)
    {
        send_message(
            socket_fd,
            MSG_ERROR,
            "LOGIN_REQUIRED"
        );

        return 0;
    }

    if (!is_valid_username(payload))
    {
        send_message(
            socket_fd,
            MSG_ERROR,
            "INVALID_USERNAME"
        );

        return 0;
    }

    strncpy(
        username,
        payload,
        username_size - 1
    );

    username[
        username_size - 1
    ] = '\0';

    /*
     * Username uniqueness checking and registry
     * insertion are performed atomically inside
     * client_manager_add().
     */
    int client_id = client_manager_add(
        socket_fd,
        pthread_self(),
        username
    );

    if (client_id == -2)
    {
        send_message(
            socket_fd,
            MSG_ERROR,
            "USERNAME_ALREADY_EXISTS"
        );

        return 0;
    }

    if (client_id < 0)
    {
        send_message(
            socket_fd,
            MSG_ERROR,
            "SERVER_CLIENT_LIMIT"
        );

        return -1;
    }

    /*
     * The client is registered from this point onward.
     *
     * Therefore LOGIN_SUCCESS must use the serialized
     * client_manager_send() path.
     */
    char response[USERNAME_MAX_SIZE + 32];

    int written = snprintf(
        response,
        sizeof(response),
        "LOGIN_SUCCESS %s",
        username
    );

    if (written < 0 ||
        (size_t)written >= sizeof(response))
    {
        client_manager_remove(socket_fd);

        return -1;
    }

    if (client_manager_send(
            socket_fd,
            MSG_RESPONSE,
            response
        ) != 0)
    {
        fprintf(
            stderr,
            "Client %d: failed to send login success response.\n",
            client_id
        );

        client_manager_remove(socket_fd);

        return -1;
    }

    return client_id;
}


void *client_handler(void *argument)
{
    client_context_t *context =
        (client_context_t *)argument;

    int socket_fd;

    int client_id = 0;

    int client_registered = 0;

    char username[USERNAME_MAX_SIZE];

    char payload[MESSAGE_MAX_SIZE + 1];

    uint32_t message_type;

    int remove_result;


    if (context == NULL)
    {
        return NULL;
    }

    socket_fd = context->socket_fd;

    /*
     * Ownership of the dynamically allocated context
     * belongs to this client thread.
     */
    free(context);


    printf(
        "Client thread [%lu] started.\n",
        (unsigned long)pthread_self()
    );


    memset(
        username,
        0,
        sizeof(username)
    );


    /*
     * LOGIN PHASE
     */
    client_id = perform_login(
        socket_fd,
        username,
        sizeof(username)
    );

    if (client_id <= 0)
    {
        close(socket_fd);

        printf(
            "Client thread [%lu] terminated.\n",
            (unsigned long)pthread_self()
        );

        return NULL;
    }

    client_registered = 1;


    printf(
        "Client %d registered: %s\n",
        client_id,
        username
    );


    /*
     * PERSISTENT MESSAGE LOOP
     */
    while (1)
    {
        int result = receive_message(
            socket_fd,
            &message_type,
            payload,
            sizeof(payload)
        );


        /*
         * Client closed TCP connection without sending
         * an explicit MSG_DISCONNECT.
         */
        if (result == 1)
        {
            printf(
                "Client %d (%s) closed the connection.\n",
                client_id,
                username
            );

            break;
        }


        if (result < 0)
        {
            fprintf(
                stderr,
                "Client %d (%s): receive/protocol error.\n",
                client_id,
                username
            );

            break;
        }


        switch (message_type)
        {
            /*
             * PUBLIC CHAT
             */
            case MSG_CHAT:
            {
                /*
                 * Empty chat messages are rejected.
                 */
                if (payload[0] == '\0')
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "EMPTY_MESSAGE"
                    );

                    break;
                }


                /*
                 * Construct:
                 *
                 *     username: message
                 *
                 * The resulting broadcast itself must
                 * remain within MESSAGE_MAX_SIZE.
                 */
                char broadcast_message[
                    MESSAGE_MAX_SIZE + 1
                ];

                int written = snprintf(
                    broadcast_message,
                    sizeof(broadcast_message),
                    "%s: %s",
                    username,
                    payload
                );

                if (written < 0 ||
                    (size_t)written >=
                        sizeof(broadcast_message))
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "MESSAGE_TOO_LONG"
                    );

                    break;
                }


                int recipients =
                    client_manager_broadcast(
                        socket_fd,
                        MSG_BROADCAST,
                        broadcast_message
                    );


                if (recipients < 0)
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "BROADCAST_FAILED"
                    );

                    fprintf(
                        stderr,
                        "Client %d (%s): broadcast failed.\n",
                        client_id,
                        username
                    );

                    break;
                }


                printf(
                    "Broadcast from Client %d (%s): %s\n",
                    client_id,
                    username,
                    payload
                );


                /*
                 * Send delivery count to the sender.
                 *
                 * This also uses the per-client send
                 * mutex because another client thread
                 * may simultaneously broadcast to this
                 * socket.
                 */
                char response[64];

                written = snprintf(
                    response,
                    sizeof(response),
                    "MESSAGE_DELIVERED %d",
                    recipients
                );

                if (written >= 0 &&
                    (size_t)written <
                        sizeof(response))
                {
                    if (client_manager_send(
                            socket_fd,
                            MSG_RESPONSE,
                            response
                        ) != 0)
                    {
                        fprintf(
                            stderr,
                            "Client %d: failed to send delivery response.\n",
                            client_id
                        );
                    }
                }

                break;
            }


            /*
             * GRACEFUL DISCONNECT
             */
            case MSG_DISCONNECT:
            {
                printf(
                    "Client %d (%s) requested disconnect.\n",
                    client_id,
                    username
                );


                /*
                 * Send GOODBYE while the client is still
                 * registered. Removal happens afterward.
                 */
                if (client_manager_send(
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
	    }

	                /*
             * ONLINE USER LIST
             */
            case MSG_LIST_USERS:
            {
                char user_list[
                    MESSAGE_MAX_SIZE + 1
                ];

                int users =
                    client_manager_build_userlist(
                        user_list,
                        sizeof(user_list)
                    );

                if (users < 0)
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "USERLIST_FAILED"
                    );

                    break;
                }

                if (users == 0)
                {
                    strncpy(
                        user_list,
                        "No users online",
                        sizeof(user_list) - 1
                    );

                    user_list[
                        sizeof(user_list) - 1
                    ] = '\0';
                }

                if (client_manager_send(
                        socket_fd,
                        MSG_USERLIST,
                        user_list
                    ) != 0)
                {
                    fprintf(
                        stderr,
                        "Client %d (%s): "
                        "failed to send user list.\n",
                        client_id,
                        username
                    );
                }

                break;
            }


            /*
             * FILE SHARING PLACEHOLDERS
             *
             * Protocol message types already exist,
             * but the actual transfer subsystem will be
             * implemented in a later milestone.
             */
            case MSG_UPLOAD:
            {
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "UPLOAD_NOT_IMPLEMENTED"
                );

                break;
            }


            case MSG_DOWNLOAD:
            {
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "DOWNLOAD_NOT_IMPLEMENTED"
                );

                break;
            }


            case MSG_LIST_FILES:
            {
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "LIST_FILES_NOT_IMPLEMENTED"
                );

                break;
            }


            /*
             * A client may not log in a second time
             * after the session has been established.
             */
            case MSG_LOGIN:
            {
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "ALREADY_LOGGED_IN"
                );

                break;
            }


            /*
             * These are server-originated message types.
             * Receiving them from a client is considered
             * invalid application behavior.
             */
            case MSG_RESPONSE:
            case MSG_ERROR:
            case MSG_BROADCAST:
            case MSG_USERLIST:
            {
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "INVALID_CLIENT_MESSAGE_TYPE"
                );

                break;
            }


            default:
            {
                /*
                 * Normally unreachable because
                 * receive_message() validates the type.
                 */
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "UNKNOWN_MESSAGE_TYPE"
                );

                break;
            }
        }
    }


connection_end:

    /*
     * Remove the connection from the shared registry
     * before closing the file descriptor.
     *
     * client_manager_remove() synchronizes with any
     * server thread currently sending to this client.
     */
    if (client_registered)
    {
        remove_result =
            client_manager_remove(
                socket_fd
            );

        if (remove_result == 0)
        {
            printf(
                "Client %d (%s) removed from client manager.\n",
                client_id,
                username
            );
        }
        else
        {
            fprintf(
                stderr,
                "Client %d (%s): client manager removal failed.\n",
                client_id,
                username
            );
        }


        printf(
            "Active clients: %zu\n",
            client_manager_count()
        );
    }


    /*
     * close() happens only after registry removal so
     * another connection cannot reuse this descriptor
     * while the old client is still considered active.
     */
    close(socket_fd);


    printf(
        "Client thread [%lu] terminated.\n",
        (unsigned long)pthread_self()
    );


    return NULL;
}
