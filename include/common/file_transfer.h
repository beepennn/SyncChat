#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stddef.h>

#define FILE_CHUNK_SIZE 8192

#define FILE_NAME_MAX_SIZE 128

#define FILE_MAX_SIZE \
    (50ULL * 1024ULL * 1024ULL)

#define FILE_STORAGE_DIRECTORY "storage"

/*
 * Validate a filename that will be stored on
 * the server.
 *
 * Valid examples:
 *
 * report.pdf
 * image_01.png
 * project-file.txt
 *
 * Invalid examples:
 *
 * ../secret
 * /etc/passwd
 * file/name.txt
 * .hidden
 */
int is_valid_shared_filename(
    const char *filename
);

#endif
