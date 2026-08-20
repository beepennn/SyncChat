#ifndef DOWNLOAD_CLIENT_H
#define DOWNLOAD_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Detect and process a DOWNLOAD_READY response.
 *
 * Return:
 *   0  not a download message
 *   1  download completed successfully
 *   2  download was received but local save failed
 *  -1  TCP stream became unusable
 */
int download_client_handle_server_message(
    int socket_fd,
    uint32_t message_type,
    const char *payload,
    char *result,
    size_t result_size
);

#endif
