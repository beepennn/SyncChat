#ifndef FILE_LOCK_H
#define FILE_LOCK_H

typedef enum
{
    FILE_LOCK_SHARED = 0,
    FILE_LOCK_EXCLUSIVE

} file_lock_mode_t;

/*
 * Acquire a per-filename advisory lock.
 *
 * FILE_LOCK_SHARED:
 *     used by downloads/readers
 *
 * FILE_LOCK_EXCLUSIVE:
 *     used by uploads/writers
 *
 * Return:
 *   >= 0  lock descriptor
 *    -1   failure
 *
 * The descriptor must remain open for the complete
 * protected operation.
 */
int file_lock_acquire(
    const char *filename,
    file_lock_mode_t mode
);

/*
 * Explicitly unlock and close a descriptor returned
 * by file_lock_acquire().
 */
int file_lock_release(
    int lock_fd
);

#endif
