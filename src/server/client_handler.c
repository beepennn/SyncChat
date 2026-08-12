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

    /*
     * Validate the message type before processing it.
     */
    if (!is_valid_message_type(type))
    {
        fprintf(
            stderr,
            "Invalid message type: %u\n",
            type
        );

        return -1;
    }

    /*
     * Prevent oversized payloads.
     */
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
     * Ensure the destination buffer can store
     * the payload plus the terminating '\0'.
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
 * Handle one connected client.
 *
 * Ownership of the client socket is transferred
 * to this thread.
 */
void *client_handler(void *argument)
{
    client_context_t *context = argument;

    if (context == NULL)
    {
        return NULL;
    }

    /*
     * Extract socket descriptor before freeing
     * the dynamically allocated context.
     */
    int socket_fd = context->socket_fd;

    free(context);
    context = NULL;

    pthread_t thread_id = pthread_self();

    printf(
        "Client thread [%lu] started.\n",
        (unsigned long)thread_id
    );

    /*
     * Register the client in the shared client registry.
     *
     * The client manager protects the registry using
     * its internal mutex.
     */
    int client_id = client_manager_add(
        socket_fd,
        thread_id
    );

    if (client_id < 0)
    {
        fprintf(
            stderr,
            "Client thread [%lu]: "
            "failed to register client.\n",
            (unsigned long)thread_id
        );

        close(socket_fd);

        return NULL;
    }

    printf(
        "Client %d registered in client manager.\n",
        client_id
    );

    char payload[MESSAGE_MAX_SIZE + 1];
    uint32_t message_type;

    /*
     * Send greeting to the client.
     */
    if (send_message(
            socket_fd,
            MSG_CHAT,
            "Welcome to SyncChat server."
        ) != 0)
    {
        fprintf(
            stderr,
            "Client %d: failed to send greeting.\n",
            client_id
        );

        /*
         * Remove the client before closing the socket
         * so the registry reflects the connection state.
         */
        int remove_result =
            client_manager_remove(socket_fd);

        if (remove_result != 0)
        {
            fprintf(
                stderr,
                "Client %d: failed to remove "
                "client from registry.\n",
                client_id
            );
        }

        close(socket_fd);

        return NULL;
    }

    /*
     * Receive one framed application message.
     */
    int result = receive_message(
        socket_fd,
        &message_type,
        payload,
        sizeof(payload)
    );

    if (result == 1)
    {
        printf(
            "Client %d: client disconnected.\n",
            client_id
        );
    }
    else if (result < 0)
    {
        fprintf(
            stderr,
            "Client %d: receive error.\n",
            client_id
        );
    }
    else
    {
        printf(
            "Client %d: message type=%u, message=%s\n",
            client_id,
            message_type,
            payload
        );
    }

    /*
     * Remove the client from the shared registry
     * before closing its socket.
     */
    int remove_result =
        client_manager_remove(socket_fd);

    if (remove_result == 0)
    {
        printf(
            "Client %d removed from client manager.\n",
            client_id
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
            "Client %d: failed to update "
            "client registry.\n",
            client_id
        );
    }

    /*
     * Now release the socket resource.
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
