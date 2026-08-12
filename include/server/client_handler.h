#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

typedef struct
{
    int socket_fd;
} client_context_t;

void *client_handler(void *argument);

#endif
