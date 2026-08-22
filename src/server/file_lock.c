#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/file_transfer.h"
#include "server/file_lock.h"

#define FILE_LOCK_DIRECTORY \
    FILE_STORAGE_DIRECTORY "/.locks"

#ifndef F_OFD_SETLK
#error "SyncChat requires Linux open-file-description fcntl locks"
#endif

#ifndef F_OFD_SETLKW
#error "SyncChat requires Linux open-file-description fcntl locks"
#endif


static int ensure_lock_directory(void)
{
    struct stat information;


    if (mkdir(
            FILE_LOCK_DIRECTORY,
            0700
        ) == 0)
    {
        return 0;
    }


    if (errno != EEXIST)
    {
        return -1;
    }


    /*
     * lstat() deliberately does not follow a symbolic
     * link. The internal lock directory itself must be
     * a real directory owned by the SyncChat storage
     * layout.
     */
    if (lstat(
            FILE_LOCK_DIRECTORY,
            &information
        ) != 0)
    {
        return -1;
    }


    if (!S_ISDIR(
            information.st_mode
        ))
    {
        errno = ENOTDIR;

        return -1;
    }


    return 0;
}


static int open_lock_directory(void)
{
    int flags =
        O_RDONLY |
        O_DIRECTORY |
        O_CLOEXEC;


#ifdef O_NOFOLLOW
    flags |=
        O_NOFOLLOW;
#endif


    return open(
        FILE_LOCK_DIRECTORY,
        flags
    );
}


int file_lock_acquire(
    const char *filename,
    file_lock_mode_t mode
)
{
    if (filename == NULL ||
        !is_valid_shared_filename(
            filename
        ))
    {
        errno = EINVAL;

        return -1;
    }


    if (mode != FILE_LOCK_SHARED &&
        mode != FILE_LOCK_EXCLUSIVE)
    {
        errno = EINVAL;

        return -1;
    }


    if (ensure_lock_directory() != 0)
    {
        return -1;
    }


    int directory_fd =
        open_lock_directory();


    if (directory_fd < 0)
    {
        return -1;
    }


    char lock_name[
        FILE_NAME_MAX_SIZE + 8
    ];


    int name_written =
        snprintf(
            lock_name,
            sizeof(lock_name),
            "%s.lock",
            filename
        );


    if (name_written < 0 ||
        (size_t)name_written >=
            sizeof(lock_name))
    {
        close(directory_fd);

        errno = ENAMETOOLONG;

        return -1;
    }


    int open_flags =
        O_RDWR |
        O_CREAT |
        O_CLOEXEC;


#ifdef O_NOFOLLOW
    open_flags |=
        O_NOFOLLOW;
#endif


    /*
     * openat() keeps the lock-file lookup confined to
     * the already-open .locks directory.
     */
    int lock_fd =
        openat(
            directory_fd,
            lock_name,
            open_flags,
            0600
        );


    int saved_errno =
        errno;


    if (close(
            directory_fd
        ) != 0 &&
        lock_fd >= 0)
    {
        close(lock_fd);

        return -1;
    }


    if (lock_fd < 0)
    {
        errno =
            saved_errno;

        return -1;
    }


    struct stat information;


    if (fstat(
            lock_fd,
            &information
        ) != 0 ||
        !S_ISREG(
            information.st_mode
        ))
    {
        saved_errno =
            errno;


        if (saved_errno == 0)
        {
            saved_errno =
                EINVAL;
        }


        close(lock_fd);

        errno =
            saved_errno;

        return -1;
    }


    struct flock lock;


    memset(
        &lock,
        0,
        sizeof(lock)
    );


    lock.l_type =
        mode == FILE_LOCK_SHARED
            ? F_RDLCK
            : F_WRLCK;

    lock.l_whence =
        SEEK_SET;

    lock.l_start = 0;
    lock.l_len = 0;


    while (fcntl(
            lock_fd,
            F_OFD_SETLKW,
            &lock
        ) != 0)
    {
        if (errno == EINTR)
        {
            continue;
        }


        saved_errno =
            errno;


        close(lock_fd);


        errno =
            saved_errno;


        return -1;
    }


    return lock_fd;
}


int file_lock_release(
    int lock_fd
)
{
    if (lock_fd < 0)
    {
        errno = EINVAL;

        return -1;
    }


    struct flock lock;


    memset(
        &lock,
        0,
        sizeof(lock)
    );


    lock.l_type =
        F_UNLCK;

    lock.l_whence =
        SEEK_SET;

    lock.l_start = 0;
    lock.l_len = 0;


    int unlock_result;


    do
    {
        unlock_result =
            fcntl(
                lock_fd,
                F_OFD_SETLK,
                &lock
            );
    }
    while (unlock_result != 0 &&
           errno == EINTR);


    int saved_errno =
        errno;


    int close_result =
        close(
            lock_fd
        );


    if (unlock_result != 0)
    {
        errno =
            saved_errno;

        return -1;
    }


    if (close_result != 0)
    {
        return -1;
    }


    return 0;
}
