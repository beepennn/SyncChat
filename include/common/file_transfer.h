#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stddef.h>

#define FILE_CHUNK_SIZE 8192
#define FILE_NAME_MAX_SIZE 128
#define FILE_MAX_SIZE \
    (50ULL * 1024ULL * 1024ULL)

#define FILE_STORAGE_DIRECTORY "storage"
#define FILE_DOWNLOAD_DIRECTORY "downloads"

/*
 * Validate a server-visible shared filename.
 *
 * Security policy:
 * - basename only
 * - no slash or backslash
 * - no hidden names
 * - no ".." sequence
 * - only letters, digits, underscore, hyphen, dot
 */
int is_valid_shared_filename(
    const char *filename
);

#endif
