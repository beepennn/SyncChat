#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

/*
 * Handle one client upload request.
 *
 * metadata:
 *
 *     filename|filesize
 */
int file_manager_handle_upload(
    int socket_fd,
    const char *username,
    const char *metadata
);


/*
 * Handle one download request.
 *
 * filename contains only the shared filename,
 * never a path.
 */
int file_manager_handle_download(
    int socket_fd,
    const char *username,
    const char *filename
);


/*
 * Send the current list of completed, downloadable
 * regular files to one client.
 */
int file_manager_handle_list(
    int socket_fd,
    const char *username
);

#endif
