#ifndef NETWORK_IO_H
#define NETWORK_IO_H

#include <stddef.h>
#include <sys/types.h>

int send_all(int socket_fd, const void *buffer, size_t length);

int recv_all(int socket_fd, void *buffer, size_t length);

#endif
