#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#include <pthread.h>
#include <stddef.h>

#define MAX_CLIENTS 64
#define USERNAME_MAX_SIZE 32

typedef struct
{
    int active;
    int socket_fd;
    pthread_t thread_id;
    unsigned int client_id;
} client_entry_t;


/*
 * Initialize the client manager.
 *
 * Returns:
 *   0  on success
 *  -1  on failure
 */
int client_manager_init(void);


/*
 * Register a connected client.
 *
 * Returns:
 *   >= 0 : assigned client ID
 *   -1   : registry is full or an error occurred
 */
int client_manager_add(
    int socket_fd,
    pthread_t thread_id
);


/*
 * Remove a client using its socket descriptor.
 *
 * Returns:
 *   0  client removed
 *   1  client was not found
 *  -1  mutex/error failure
 */
int client_manager_remove(int socket_fd);


/*
 * Return the number of active clients.
 */
size_t client_manager_count(void);


/*
 * Check whether a socket belongs to an active client.
 *
 * Returns:
 *   1  active
 *   0  not active
 */
int client_manager_contains(int socket_fd);

#endif
