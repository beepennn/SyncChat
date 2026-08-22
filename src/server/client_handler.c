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
#include "logger/logger_client.h"
#include "server/client_handler.h"
#include "server/client_manager.h"
#include "server/file_manager.h"

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


    payload_length =
        strlen(payload);


    if (payload_length >
        MESSAGE_MAX_SIZE)
    {
        return -1;
    }


    header.type =
        htonl(type);


    header.length =
        htonl(
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


    if (message_type == NULL ||
        payload == NULL ||
        payload_size == 0)
    {
        return -1;
    }


    int result = recv_all(
        socket_fd,
        &header,
        sizeof(header)
    );


    if (result != 0)
    {
        return result;
    }


    type =
        ntohl(header.type);

    length =
        ntohl(header.length);


    if (!is_valid_message_type(type))
    {
        logger_client_log(
            LOG_WARN,
            "CLIENT_HANDLER",
            "Rejected invalid message type %u on socket %d",
            type,
            socket_fd
        );

        return -1;
    }


    if (length > MESSAGE_MAX_SIZE)
    {
        logger_client_log(
            LOG_WARN,
            "CLIENT_HANDLER",
            "Rejected oversized payload length=%u socket=%d",
            length,
            socket_fd
        );

        return -1;
    }


    if ((size_t)length >=
        payload_size)
    {
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


static int perform_login(
    int socket_fd,
    char *username,
    size_t username_size
)
{
    char payload[
        MESSAGE_MAX_SIZE + 1
    ];

    uint32_t message_type;


    if (username == NULL ||
        username_size <
            USERNAME_MAX_SIZE)
    {
        return -1;
    }


    if (send_message(
            socket_fd,
            MSG_RESPONSE,
            "USERNAME_REQUIRED"
        ) != 0)
    {
        logger_client_log(
            LOG_ERROR,
            "AUTH",
            "Failed to request username socket=%d",
            socket_fd
        );

        return -1;
    }


    int result =
        receive_message(
            socket_fd,
            &message_type,
            payload,
            sizeof(payload)
        );


    if (result != 0)
    {
        logger_client_log(
            LOG_WARN,
            "AUTH",
            "Client disconnected or failed during login socket=%d",
            socket_fd
        );

        return -1;
    }


    if (message_type !=
        MSG_LOGIN)
    {
        send_message(
            socket_fd,
            MSG_ERROR,
            "LOGIN_REQUIRED"
        );


        logger_client_log(
            LOG_WARN,
            "AUTH",
            "Rejected non-login message during authentication socket=%d",
            socket_fd
        );

        return 0;
    }


    if (!is_valid_username(
            payload
        ))
    {
        send_message(
            socket_fd,
            MSG_ERROR,
            "INVALID_USERNAME"
        );


        logger_client_log(
            LOG_WARN,
            "AUTH",
            "Rejected invalid username socket=%d",
            socket_fd
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


    int client_id =
        client_manager_add(
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


        logger_client_log(
            LOG_WARN,
            "AUTH",
            "Duplicate username rejected username=%s",
            username
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


        logger_client_log(
            LOG_ERROR,
            "AUTH",
            "Unable to register username=%s",
            username
        );

        return -1;
    }


    char response[
        USERNAME_MAX_SIZE + 32
    ];


    int written =
        snprintf(
            response,
            sizeof(response),
            "LOGIN_SUCCESS %s",
            username
        );


    if (written < 0 ||
        (size_t)written >=
            sizeof(response))
    {
        client_manager_remove(
            socket_fd
        );

        return -1;
    }


    if (client_manager_send(
            socket_fd,
            MSG_RESPONSE,
            response
        ) != 0)
    {
        logger_client_log(
            LOG_ERROR,
            "AUTH",
            "Failed to send login success client_id=%d username=%s",
            client_id,
            username
        );


        client_manager_remove(
            socket_fd
        );

        return -1;
    }


    logger_client_log(
        LOG_INFO,
        "AUTH",
        "Login success client_id=%d username=%s",
        client_id,
        username
    );


    return client_id;
}


void *client_handler(
    void *argument
)
{
    client_context_t *context =
        (client_context_t *)argument;


    if (context == NULL)
    {
        return NULL;
    }


    int socket_fd =
        context->socket_fd;


    free(context);


    int client_id = 0;

    int client_registered = 0;

    int remove_result;


    char username[
        USERNAME_MAX_SIZE
    ];


    char payload[
        MESSAGE_MAX_SIZE + 1
    ];


    uint32_t message_type;


    printf(
        "Client thread [%lu] started.\n",
        (unsigned long)pthread_self()
    );


    logger_client_log(
        LOG_DEBUG,
        "CLIENT_HANDLER",
        "Client thread started thread=%lu socket=%d",
        (unsigned long)pthread_self(),
        socket_fd
    );


    memset(
        username,
        0,
        sizeof(username)
    );


    client_id =
        perform_login(
            socket_fd,
            username,
            sizeof(username)
        );


    if (client_id <= 0)
    {
        close(socket_fd);


        logger_client_log(
            LOG_DEBUG,
            "CLIENT_HANDLER",
            "Unregistered client thread terminated thread=%lu",
            (unsigned long)pthread_self()
        );


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


    while (1)
    {
        int result =
            receive_message(
                socket_fd,
                &message_type,
                payload,
                sizeof(payload)
            );


        if (result == 1)
        {
            printf(
                "Client %d (%s) closed the connection.\n",
                client_id,
                username
            );


            logger_client_log(
                LOG_INFO,
                "CLIENT_HANDLER",
                "Unexpected disconnect client_id=%d username=%s",
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


            logger_client_log(
                LOG_WARN,
                "CLIENT_HANDLER",
                "Receive/protocol error client_id=%d username=%s",
                client_id,
                username
            );


            break;
        }


        switch (message_type)
        {
            case MSG_CHAT:
            {
                if (payload[0] == '\0')
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "EMPTY_MESSAGE"
                    );

                    break;
                }


                char broadcast_message[
                    MESSAGE_MAX_SIZE + 1
                ];


                int written =
                    snprintf(
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


                    logger_client_log(
                        LOG_ERROR,
                        "CLIENT_HANDLER",
                        "Broadcast failed client_id=%d username=%s",
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
                 * Deliberately do not record message
                 * contents in the server log.
                 */
                logger_client_log(
                    LOG_INFO,
                    "CLIENT_HANDLER",
                    "Broadcast sender=%s recipients=%d",
                    username,
                    recipients
                );


                char response[64];


                written =
                    snprintf(
                        response,
                        sizeof(response),
                        "MESSAGE_DELIVERED %d",
                        recipients
                    );


                if (written >= 0 &&
                    (size_t)written <
                        sizeof(response))
                {
                    client_manager_send(
                        socket_fd,
                        MSG_RESPONSE,
                        response
                    );
                }


                break;
            }


            case MSG_PRIVATE:
            {
                char *separator =
                    strchr(
                        payload,
                        ' '
                    );


                if (separator == NULL ||
                    separator == payload ||
                    separator[1] == '\0')
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "PRIVATE_MESSAGE_USAGE"
                    );

                    break;
                }


                *separator = '\0';


                const char *target_username =
                    payload;


                const char *private_text =
                    separator + 1;


                if (!is_valid_username(
                        target_username
                    ))
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "INVALID_TARGET_USERNAME"
                    );

                    break;
                }


                if (strcmp(
                        target_username,
                        username
                    ) == 0)
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "CANNOT_MESSAGE_SELF"
                    );

                    break;
                }


                char private_message[
                    MESSAGE_MAX_SIZE + 1
                ];


                int written =
                    snprintf(
                        private_message,
                        sizeof(private_message),
                        "%s: %s",
                        username,
                        private_text
                    );


                if (written < 0 ||
                    (size_t)written >=
                        sizeof(private_message))
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "PRIVATE_MESSAGE_TOO_LONG"
                    );

                    break;
                }


                int send_result =
                    client_manager_send_to_username(
                        target_username,
                        MSG_PRIVATE,
                        private_message
                    );


                if (send_result == 1)
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "USER_NOT_FOUND"
                    );


                    logger_client_log(
                        LOG_WARN,
                        "CLIENT_HANDLER",
                        "Private message target unavailable sender=%s target=%s",
                        username,
                        target_username
                    );


                    break;
                }


                if (send_result < 0)
                {
                    client_manager_send(
                        socket_fd,
                        MSG_ERROR,
                        "PRIVATE_MESSAGE_FAILED"
                    );


                    logger_client_log(
                        LOG_ERROR,
                        "CLIENT_HANDLER",
                        "Private message delivery failed sender=%s target=%s",
                        username,
                        target_username
                    );


                    break;
                }


                char response[
                    USERNAME_MAX_SIZE + 40
                ];


                written =
                    snprintf(
                        response,
                        sizeof(response),
                        "PRIVATE_MESSAGE_DELIVERED %s",
                        target_username
                    );


                if (written >= 0 &&
                    (size_t)written <
                        sizeof(response))
                {
                    client_manager_send(
                        socket_fd,
                        MSG_RESPONSE,
                        response
                    );
                }


                printf(
                    "Private message from Client %d (%s) to %s.\n",
                    client_id,
                    username,
                    target_username
                );


                logger_client_log(
                    LOG_INFO,
                    "CLIENT_HANDLER",
                    "Private message sender=%s recipient=%s",
                    username,
                    target_username
                );


                break;
            }


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


                    logger_client_log(
                        LOG_ERROR,
                        "CLIENT_HANDLER",
                        "User list generation failed requester=%s",
                        username
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


                client_manager_send(
                    socket_fd,
                    MSG_USERLIST,
                    user_list
                );


                logger_client_log(
                    LOG_DEBUG,
                    "CLIENT_HANDLER",
                    "User list requested username=%s count=%d",
                    username,
                    users
                );


                break;
            }


            case MSG_DISCONNECT:
            {
                printf(
                    "Client %d (%s) requested disconnect.\n",
                    client_id,
                    username
                );


                logger_client_log(
                    LOG_INFO,
                    "CLIENT_HANDLER",
                    "Graceful disconnect requested client_id=%d username=%s",
                    client_id,
                    username
                );


                client_manager_send(
                    socket_fd,
                    MSG_RESPONSE,
                    "GOODBYE"
                );


                goto connection_end;
            }


            case MSG_UPLOAD:
            {
                int upload_result =
                    file_manager_handle_upload(
                        socket_fd,
                        username,
                        payload
                    );

                /*
                 * A negative result means the TCP
                 * stream became unusable during
                 * transfer.
                 */
                if (upload_result < 0)
                {
                    goto connection_end;
                }

                break;
            }


            case MSG_DOWNLOAD:
            {
                int download_result =
                    file_manager_handle_download(
                        socket_fd,
                        username,
                        payload
                    );


                if (download_result < 0)
                {
                    goto connection_end;
                }


                break;
            }


            case MSG_LIST_FILES:
            {
                int list_result =
                    file_manager_handle_list(
                        socket_fd,
                        username
                    );


                if (list_result < 0)
                {
                    goto connection_end;
                }


                break;
            }


            case MSG_LOGIN:
            {
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "ALREADY_LOGGED_IN"
                );


                logger_client_log(
                    LOG_WARN,
                    "CLIENT_HANDLER",
                    "Repeated login attempt username=%s",
                    username
                );


                break;
            }


            case MSG_RESPONSE:
            case MSG_ERROR:
            case MSG_BROADCAST:
            case MSG_USERLIST:
            case MSG_FILELIST:
            {
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "INVALID_CLIENT_MESSAGE_TYPE"
                );


                logger_client_log(
                    LOG_WARN,
                    "CLIENT_HANDLER",
                    "Invalid client-originated message type=%u username=%s",
                    message_type,
                    username
                );


                break;
            }


            default:
            {
                client_manager_send(
                    socket_fd,
                    MSG_ERROR,
                    "UNKNOWN_MESSAGE_TYPE"
                );


                logger_client_log(
                    LOG_WARN,
                    "CLIENT_HANDLER",
                    "Unknown message type=%u username=%s",
                    message_type,
                    username
                );


                break;
            }
        }
    }


connection_end:

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


            logger_client_log(
                LOG_ERROR,
                "CLIENT_HANDLER",
                "Client manager removal failed client_id=%d username=%s",
                client_id,
                username
            );
        }


        size_t active_clients =
            client_manager_count();


        printf(
            "Active clients: %zu\n",
            active_clients
        );


        logger_client_log(
            LOG_INFO,
            "CLIENT_HANDLER",
            "Client removed client_id=%d username=%s active_clients=%zu",
            client_id,
            username,
            active_clients
        );
    }


    close(socket_fd);


    logger_client_log(
        LOG_DEBUG,
        "CLIENT_HANDLER",
        "Client thread terminated thread=%lu username=%s",
        (unsigned long)pthread_self(),
        username
    );


    printf(
        "Client thread [%lu] terminated.\n",
        (unsigned long)pthread_self()
    );


    return NULL;
}
