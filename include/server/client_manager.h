#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "common/protocol.h"

#define MAX_CLIENTS 64

typedef struct
{
    int active;
    int socket_fd;

    pthread_t thread_id;

    unsigned int client_id;

    char username[USERNAME_MAX_SIZE];

    /*
     * Serializes all server-side writes to this
     * client's TCP stream.
     */
    pthread_mutex_t send_mutex;

} client_entry_t;


/*
 * Initialize the shared client registry.
 */
int client_manager_init(void);


/*
 * Register a logged-in client.
 *
 * Return:
 *   >0  assigned client ID
 *   -1  registry/full/internal error
 *   -2  username already exists
 */
int client_manager_add(
    int socket_fd,
    pthread_t thread_id,
    const char *username
);


/*
 * Remove a registered client.
 *
 * Return:
 *   0  removed
 *   1  client not found
 *  -1  internal error
 */
int client_manager_remove(int socket_fd);


size_t client_manager_count(void);


int client_manager_contains(int socket_fd);


int client_manager_username_exists(
    const char *username
);


int client_manager_get_username(
    int socket_fd,
    char *username,
    size_t username_size
);


/*
 * Thread-safe framed send to one registered client.
 *
 * All post-login server writes should use this
 * function instead of writing directly to the socket.
 */
int client_manager_send(
    int socket_fd,
    uint32_t message_type,
    const char *payload
);


/*
 * Broadcast one framed message to every active
 * client except sender_socket.
 *
 * Returns the number of successful recipients,
 * or -1 on manager/protocol failure.
 */
int client_manager_broadcast(
    int sender_socket,
    uint32_t message_type,
    const char *payload
);

#endif
