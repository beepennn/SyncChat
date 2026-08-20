#ifndef UPLOAD_CLIENT_H
#define UPLOAD_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Upload a local file using an existing connected
 * SyncChat TCP socket.
 *
 * Return:
 *   0  upload successful
 *   1  upload rejected by server/local validation
 *  -1  connection became unusable
 */
int upload_client_upload(
    int socket_fd,
    const char *local_path,
    char *result,
    size_t result_size
);


/*
 * Called by the receiver thread.
 *
 * Returns 1 when the message belongs to the upload
 * state machine and should not be printed by the
 * normal receiver switch.
 */
int upload_client_handle_server_message(
    uint32_t message_type,
    const char *payload
);


/*
 * Wake an upload operation if the connection closes.
 */
void upload_client_notify_connection_closed(void);

#endif
