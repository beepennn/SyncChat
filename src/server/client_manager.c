#include <arpa/inet.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "common/network_io.h"
#include "common/protocol.h"
#include "server/client_manager.h"
#include "common/file_transfer.h"


typedef struct
{
    int slot;
    unsigned int client_id;

} broadcast_target_t;


static client_entry_t clients[MAX_CLIENTS];

static pthread_mutex_t clients_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static unsigned int next_client_id = 1;

static int manager_initialized = 0;


/*
 * Caller must hold clients_mutex.
 */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].active &&
            clients[i].socket_fd == -1)
        {
            return i;
        }
    }

    return -1;
}


/*
 * Caller must hold clients_mutex.
 */
static int find_socket_slot(int socket_fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active &&
            clients[i].socket_fd == socket_fd)
        {
            return i;
        }
    }

    return -1;
}


/*
 * Caller must hold clients_mutex.
 */
static int find_username_slot(
    const char *username
)
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


/*
 * Raw framed network send.
 *
 * The caller is responsible for ensuring that
 * only one thread writes to socket_fd at a time.
 */
static int send_message_raw(
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
 * Send to an exact registry slot/client generation.
 *
 * client_id prevents an old broadcast snapshot from
 * accidentally sending to a newly connected client
 * that later reused the same registry slot.
 */
static int send_to_target(
    int slot,
    unsigned int expected_client_id,
    uint32_t message_type,
    const char *payload
)
{
    if (slot < 0 ||
        slot >= MAX_CLIENTS)
    {
        return -1;
    }

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    /*
     * Verify that this is still the same client
     * recorded by the broadcast snapshot.
     */
    if (!clients[slot].active ||
        clients[slot].client_id != expected_client_id)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return 1;
    }

    /*
     * Lock order throughout the manager:
     *
     * clients_mutex -> send_mutex
     *
     * Never reverse this order.
     */
    if (pthread_mutex_lock(
            &clients[slot].send_mutex
        ) != 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return -1;
    }

    int socket_fd =
        clients[slot].socket_fd;

    /*
     * Release the global registry mutex before
     * performing potentially blocking network I/O.
     *
     * The per-client send mutex remains locked.
     */
    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        pthread_mutex_unlock(
            &clients[slot].send_mutex
        );

        return -1;
    }

    int result = send_message_raw(
        socket_fd,
        message_type,
        payload
    );

    pthread_mutex_unlock(
        &clients[slot].send_mutex
    );

    return result;
}


int client_manager_init(void)
{
    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    if (manager_initialized)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return 0;
    }

    memset(
        clients,
        0,
        sizeof(clients)
    );

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].socket_fd = -1;

        if (pthread_mutex_init(
                &clients[i].send_mutex,
                NULL
            ) != 0)
        {
            /*
             * Destroy mutexes that were already
             * successfully initialized.
             */
            for (int j = 0; j < i; j++)
            {
                pthread_mutex_destroy(
                    &clients[j].send_mutex
                );
            }

            pthread_mutex_unlock(
                &clients_mutex
            );

            return -1;
        }
    }

    next_client_id = 1;
    manager_initialized = 1;

    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    return 0;
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

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    /*
     * Username uniqueness check and insertion are
     * performed inside the same critical section.
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

    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    return client_id;
}


int client_manager_remove(int socket_fd)
{
    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    int slot =
        find_socket_slot(socket_fd);

    if (slot < 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return 1;
    }

    /*
     * Wait for any currently active send to this
     * socket to complete before removing it.
     *
     * The global mutex prevents another thread from
     * starting a new send while removal begins.
     */
    if (pthread_mutex_lock(
            &clients[slot].send_mutex
        ) != 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return -1;
    }

    clients[slot].active = 0;
    clients[slot].socket_fd = -1;
    clients[slot].thread_id = (pthread_t)0;
    clients[slot].client_id = 0;
    clients[slot].username[0] = '\0';

    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        pthread_mutex_unlock(
            &clients[slot].send_mutex
        );

        return -1;
    }

    pthread_mutex_unlock(
        &clients[slot].send_mutex
    );

    return 0;
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

    if (find_socket_slot(socket_fd) >= 0)
    {
        found = 1;
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

    int slot =
        find_socket_slot(socket_fd);

    if (slot < 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return 1;
    }

    strncpy(
        username,
        clients[slot].username,
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

int client_manager_build_userlist(
    char *buffer,
    size_t buffer_size
)
{
    if (buffer == NULL ||
        buffer_size == 0)
    {
        return -1;
    }

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    size_t used = 0;
    int user_count = 0;

    buffer[0] = '\0';

    for (int i = 0;
         i < MAX_CLIENTS;
         i++)
    {
        if (!clients[i].active)
        {
            continue;
        }

        size_t username_length =
            strlen(clients[i].username);

        /*
         * Space required:
         *
         * username + '\n' + final '\0'
         */
        if (used +
            username_length +
            2 >
            buffer_size)
        {
            pthread_mutex_unlock(
                &clients_mutex
            );

            return -1;
        }

        memcpy(
            buffer + used,
            clients[i].username,
            username_length
        );

        used += username_length;

        buffer[used++] = '\n';

        buffer[used] = '\0';

        user_count++;
    }

    /*
     * Remove the final newline so the receiver's
     * formatting remains clean.
     */
    if (used > 0)
    {
        buffer[used - 1] = '\0';
    }

    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    return user_count;
}


int client_manager_send(
    int socket_fd,
    uint32_t message_type,
    const char *payload
)
{
    if (payload == NULL ||
        !is_valid_message_type(message_type))
    {
        return -1;
    }

    if (strlen(payload) > MESSAGE_MAX_SIZE)
    {
        return -1;
    }

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    int slot =
        find_socket_slot(socket_fd);

    if (slot < 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return 1;
    }

    /*
     * Lock the destination stream while the registry
     * mutex guarantees that the slot cannot disappear.
     */
    if (pthread_mutex_lock(
            &clients[slot].send_mutex
        ) != 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return -1;
    }

    int destination_socket =
        clients[slot].socket_fd;

    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        pthread_mutex_unlock(
            &clients[slot].send_mutex
        );

        return -1;
    }

    int result = send_message_raw(
        destination_socket,
        message_type,
        payload
    );

    pthread_mutex_unlock(
        &clients[slot].send_mutex
    );

    return result;
}

int client_manager_send_to_username(
    const char *username,
    uint32_t message_type,
    const char *payload
)
{
    if (!is_valid_username(username) ||
        payload == NULL ||
        !is_valid_message_type(message_type))
    {
        return -1;
    }

    if (strlen(payload) > MESSAGE_MAX_SIZE)
    {
        return -1;
    }

    /*
     * Resolve the username while holding the
     * registry mutex.
     */
    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    int slot =
        find_username_slot(username);

    if (slot < 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return 1;
    }

    /*
     * Snapshot the logical client ID.
     *
     * send_to_target() will verify that this is still
     * the same connection before sending.
     */
    unsigned int client_id =
        clients[slot].client_id;

    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    return send_to_target(
        slot,
        client_id,
        message_type,
        payload
    );
}

int client_manager_send_file_stream(
    int socket_fd,
    uint32_t message_type,
    const char *metadata,
    int file_fd,
    uint64_t file_size
)
{
    if (socket_fd < 0 ||
        metadata == NULL ||
        file_fd < 0 ||
        !is_valid_message_type(message_type) ||
        strlen(metadata) > MESSAGE_MAX_SIZE ||
        file_size > FILE_MAX_SIZE)
    {
        return -1;
    }


    /*
     * Resolve the logical client while holding the
     * registry mutex.
     */
    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }


    int slot =
        find_socket_slot(
            socket_fd
        );


    if (slot < 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return -1;
    }


    unsigned int expected_client_id =
        clients[slot].client_id;


    /*
     * Lock order remains:
     *
     * clients_mutex -> send_mutex
     */
    if (pthread_mutex_lock(
            &clients[slot].send_mutex
        ) != 0)
    {
        pthread_mutex_unlock(
            &clients_mutex
        );

        return -1;
    }


    /*
     * Revalidate while registry state is protected.
     */
    if (!clients[slot].active ||
        clients[slot].socket_fd != socket_fd ||
        clients[slot].client_id !=
            expected_client_id)
    {
        pthread_mutex_unlock(
            &clients[slot].send_mutex
        );

        pthread_mutex_unlock(
            &clients_mutex
        );

        return -1;
    }


    /*
     * Registry can now be released.
     *
     * The per-client send mutex remains locked for
     * the metadata frame AND every raw file byte.
     *
     * This prevents broadcasts/private messages from
     * being inserted into the middle of file data.
     */
    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        pthread_mutex_unlock(
            &clients[slot].send_mutex
        );

        return -1;
    }


    int result =
        send_message_raw(
            socket_fd,
            message_type,
            metadata
        );


    if (result != 0)
    {
        pthread_mutex_unlock(
            &clients[slot].send_mutex
        );

        return -1;
    }


    unsigned char buffer[
        FILE_CHUNK_SIZE
    ];


    uint64_t remaining =
        file_size;


    while (remaining > 0)
    {
        size_t requested;


        if (remaining >
            FILE_CHUNK_SIZE)
        {
            requested =
                FILE_CHUNK_SIZE;
        }
        else
        {
            requested =
                (size_t)remaining;
        }


        ssize_t bytes_read =
            read(
                file_fd,
                buffer,
                requested
            );


        if (bytes_read < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }


            pthread_mutex_unlock(
                &clients[slot].send_mutex
            );

            return -1;
        }


        if (bytes_read == 0)
        {
            pthread_mutex_unlock(
                &clients[slot].send_mutex
            );

            return -1;
        }


        if (send_all(
                socket_fd,
                buffer,
                (size_t)bytes_read
            ) != 0)
        {
            pthread_mutex_unlock(
                &clients[slot].send_mutex
            );

            return -1;
        }


        remaining -=
            (uint64_t)bytes_read;
    }


    if (pthread_mutex_unlock(
            &clients[slot].send_mutex
        ) != 0)
    {
        return -1;
    }


    return 0;
}

int client_manager_broadcast(
    int sender_socket,
    uint32_t message_type,
    const char *payload
)
{
    if (payload == NULL ||
        !is_valid_message_type(message_type))
    {
        return -1;
    }

    if (strlen(payload) > MESSAGE_MAX_SIZE)
    {
        return -1;
    }

    broadcast_target_t targets[MAX_CLIENTS];

    size_t target_count = 0;

    if (pthread_mutex_lock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    /*
     * Snapshot logical client identities rather than
     * only file descriptors.
     *
     * A client ID changes when a registry slot is
     * reused, preventing stale snapshots from targeting
     * a different connection.
     */
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].active)
        {
            continue;
        }

        if (clients[i].socket_fd ==
            sender_socket)
        {
            continue;
        }

        targets[target_count].slot = i;

        targets[target_count].client_id =
            clients[i].client_id;

        target_count++;
    }

    if (pthread_mutex_unlock(
            &clients_mutex
        ) != 0)
    {
        return -1;
    }

    size_t successful = 0;

    for (size_t i = 0;
         i < target_count;
         i++)
    {
        int result = send_to_target(
            targets[i].slot,
            targets[i].client_id,
            message_type,
            payload
        );

        if (result == 0)
        {
            successful++;
        }
        else if (result < 0)
        {
            fprintf(
                stderr,
                "Broadcast send failed for client ID %u.\n",
                targets[i].client_id
            );
        }
    }

    return (int)successful;
}
