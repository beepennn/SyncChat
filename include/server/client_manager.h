#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#include <pthread.h>
#include <stddef.h>

#include "common/protocol.h"

#define MAX_CLIENTS 64

typedef struct
{
    int active;
    int socket_fd;
    pthread_t thread_id;
    unsigned int client_id;
    char username[USERNAME_MAX_SIZE];

} client_entry_t;

int client_manager_init(void);

int client_manager_add(
    int socket_fd,
    pthread_t thread_id,
    const char *username
);

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

int client_manager_broadcast(
    int sender_socket,
    uint32_t message_type,
    const char *payload
);

#endif
