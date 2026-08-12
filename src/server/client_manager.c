#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "server/client_manager.h"


/*
 * Shared client registry.
 *
 * Access to this array must always be protected
 * by clients_mutex.
 */
static client_entry_t clients[MAX_CLIENTS];


/*
 * Mutex protecting the client registry.
 */
static pthread_mutex_t clients_mutex =
    PTHREAD_MUTEX_INITIALIZER;


/*
 * Monotonically increasing client identifier.
 */
static unsigned int next_client_id = 1;


/*
 * Find an unused registry slot.
 *
 * IMPORTANT:
 * This function must only be called while
 * clients_mutex is locked.
 */
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


int client_manager_init(void)
{
    int result;

    result = pthread_mutex_lock(&clients_mutex);

    if (result != 0)
    {
        return -1;
    }

    memset(
        clients,
        0,
        sizeof(clients)
    );

    /*
     * Make all socket descriptors explicitly invalid.
     */
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].socket_fd = -1;
    }

    next_client_id = 1;

    result = pthread_mutex_unlock(&clients_mutex);

    if (result != 0)
    {
        return -1;
    }

    return 0;
}


int client_manager_add(
    int socket_fd,
    pthread_t thread_id
)
{
    int result;
    int slot;

    result = pthread_mutex_lock(&clients_mutex);

    if (result != 0)
    {
        return -1;
    }

    slot = find_free_slot();

    if (slot < 0)
    {
        /*
         * Registry is full.
         */
        pthread_mutex_unlock(&clients_mutex);
        return -1;
    }

    clients[slot].active = 1;
    clients[slot].socket_fd = socket_fd;
    clients[slot].thread_id = thread_id;
    clients[slot].client_id = next_client_id++;

    unsigned int assigned_id =
        clients[slot].client_id;

    result = pthread_mutex_unlock(&clients_mutex);

    if (result != 0)
    {
        return -1;
    }

    return (int)assigned_id;
}


int client_manager_remove(int socket_fd)
{
    int result;

    result = pthread_mutex_lock(&clients_mutex);

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

    int result = pthread_mutex_lock(
        &clients_mutex
    );

    if (result != 0)
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

    int result = pthread_mutex_lock(
        &clients_mutex
    );

    if (result != 0)
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
