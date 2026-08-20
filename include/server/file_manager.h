#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

/*
 * Process one upload request.
 *
 * metadata format:
 *
 *     filename|filesize
 *
 * Return:
 *   0  request processed
 *  -1  connection became unusable
 */
int file_manager_handle_upload(
    int socket_fd,
    const char *username,
    const char *metadata
);

#endif
