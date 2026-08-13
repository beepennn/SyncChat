#include <arpa/inet.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "common/network_io.h"
#include "common/protocol.h"
#include "server/client_manager.h"

static client_entry_t clients[MAX_CLIENTS];

static pthread_mutex_t clients_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static unsigned int next_client_id = 1;


static int find_free_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].active)
        {
            return i;
        }
    }

    return -1;
}


static int find_username_slot(const char *username)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active &&
            strcmp(
                clients[i].username,
                username
            ) == 0)
        {
            return i;
        }
    }

    return -1;
}


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


int client_manager_init(void)
{
    int result = pthread_mutex_lock(
        &clients_mutex
    );

    if (result != 0)
    {
        return -1;
    }

    memset(
        clients,
        0,
        sizeof(clients)
    );

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].socket_fd = -1;
    }

    next_client_id = 1;

    result = pthread_mutex_unlock(
        &clients_mutex
    );

    return result == 0 ? 0 : -1;
}


int client_manager_add(
    int socket_fd,
    pthread_t thread_id,
    const char *username
)
{
    if (!is_valid_username(username))
    {
        return -1;
    }

    int result = pthread_mutex_lock(
        &clients_mutex
    );

    if (result != 0)
    {
        return -1;
    }

    /*
     * Username check and insertion happen while
     * holding the same mutex.
     */
    if (find_username_slot(username) >= 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return -2;
    }

    int slot = find_free_slot();

    if (slot < 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return -1;
    }

    clients[slot].active = 1;
    clients[slot].socket_fd = socket_fd;
    clients[slot].thread_id = thread_id;
    clients[slot].client_id = next_client_id++;

    strncpy(
        clients[slot].username,
        username,
        USERNAME_MAX_SIZE - 1
    );

    clients[slot].username[
        USERNAME_MAX_SIZE - 1
    ] = '\0';

    int client_id =
        (int)clients[slot].client_id;

    result = pthread_mutex_unlock(
        &clients_mutex
    );

    return result == 0 ? client_id : -1;
}


int client_manager_remove(int socket_fd)
{
    int result = pthread_mutex_lock(
        &clients_mutex
    );

    if (result != 0)
    {
        return -1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active &&
            clients[i].socket_fd == socket_fd)
        {
            clients[i].active = 0;
            clients[i].socket_fd = -1;
            clients[i].thread_id = (pthread_t)0;
            clients[i].client_id = 0;
            clients[i].username[0] = '\0';

            result = pthread_mutex_unlock(
                &clients_mutex
            );

            return result == 0 ? 0 : -1;
        }
    }

    result = pthread_mutex_unlock(
        &clients_mutex
    );

    return result == 0 ? 1 : -1;
}


size_t client_manager_count(void)
{
    size_t count = 0;

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return 0;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active)
        {
            count++;
        }
    }

    pthread_mutex_unlock(
        &clients_mutex
    );

    return count;
}


int client_manager_contains(int socket_fd)
{
    int found = 0;

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return 0;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active &&
            clients[i].socket_fd == socket_fd)
        {
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(
        &clients_mutex
    );

    return found;
}


int client_manager_username_exists(
    const char *username
)
{
    if (!is_valid_username(username))
    {
        return 0;
    }

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return 0;
    }

    int found =
        find_username_slot(username) >= 0;

    pthread_mutex_unlock(
        &clients_mutex
    );

    return found;
}


int client_manager_get_username(
    int socket_fd,
    char *username,
    size_t username_size
)
{
    if (username == NULL ||
        username_size == 0)
    {
        return -1;
    }

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active &&
            clients[i].socket_fd == socket_fd)
        {
            strncpy(
                username,
                clients[i].username,
                username_size - 1
            );

            username[
                username_size - 1
            ] = '\0';

            pthread_mutex_unlock(
                &clients_mutex
            );

            return 0;
        }
    }

    pthread_mutex_unlock(
        &clients_mutex
    );

    return 1;
}


int client_manager_broadcast(
    int sender_socket,
    uint32_t message_type,
    const char *payload
)
{
    if (payload == NULL)
    {
        return -1;
    }

    if (strlen(payload) > MESSAGE_MAX_SIZE)
    {
        return -1;
    }

    /*
     * Snapshot target sockets while protected.
     */
    int target_sockets[MAX_CLIENTS];
    size_t target_count = 0;

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].active)
        {
            continue;
        }

        if (clients[i].socket_fd == sender_socket)
        {
            continue;
        }

        target_sockets[target_count++] =
            clients[i].socket_fd;
    }

    /*
     * IMPORTANT:
     * Network I/O is not performed while the
     * client registry mutex is held.
     */
    pthread_mutex_unlock(
        &clients_mutex
    );

    size_t successful = 0;

    /*
     * Send using the snapshot.
     */
    for (size_t i = 0;
         i < target_count;
         i++)
    {
        if (send_message(
                target_sockets[i],
                message_type,
                payload
            ) == 0)
        {
            successful++;
        }
        else
        {
            fprintf(
                stderr,
                "Broadcast failed for socket %d.\n",
                target_sockets[i]
            );
        }
    }

    return (int)successful;
}
