#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/file_transfer.h"
#include "common/network_io.h"
#include "common/protocol.h"
#include "logger/logger_client.h"
#include "server/client_manager.h"
#include "server/file_lock.h"
#include "server/file_manager.h"


static int write_all_file(
    int file_fd,
    const void *buffer,
    size_t length
)
{
    const unsigned char *bytes =
        (const unsigned char *)buffer;

    size_t written_total = 0;


    while (written_total < length)
    {
        ssize_t written =
            write(
                file_fd,
                bytes + written_total,
                length - written_total
            );


        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }


            return -1;
        }


        if (written == 0)
        {
            errno = EIO;

            return -1;
        }


        written_total +=
            (size_t)written;
    }


    return 0;
}


static int send_error(
    int socket_fd,
    const char *message
)
{
    if (client_manager_send(
            socket_fd,
            MSG_ERROR,
            message
        ) != 0)
    {
        return -1;
    }


    return 0;
}


static int ensure_storage_directory(void)
{
    struct stat information;


    /*
     * lstat() intentionally does not follow symbolic
     * links. The top-level shared storage path must be
     * a real directory, not a redirect elsewhere.
     */
    if (lstat(
            FILE_STORAGE_DIRECTORY,
            &information
        ) == 0)
    {
        if (!S_ISDIR(
                information.st_mode
            ))
        {
            errno = ENOTDIR;

            return -1;
        }


        return 0;
    }


    if (errno != ENOENT)
    {
        return -1;
    }


    if (mkdir(
            FILE_STORAGE_DIRECTORY,
            0755
        ) != 0 &&
        errno != EEXIST)
    {
        return -1;
    }


    /*
     * Re-check after creation in case another actor
     * raced with mkdir().
     */
    if (lstat(
            FILE_STORAGE_DIRECTORY,
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


static int open_storage_directory(void)
{
    int flags =
        O_RDONLY |
        O_DIRECTORY |
        O_CLOEXEC;


#ifdef O_NOFOLLOW
    flags |=
        O_NOFOLLOW;
#endif


    int directory_fd =
        open(
            FILE_STORAGE_DIRECTORY,
            flags
        );


    if (directory_fd < 0)
    {
        return -1;
    }


    struct stat information;


    if (fstat(
            directory_fd,
            &information
        ) != 0 ||
        !S_ISDIR(
            information.st_mode
        ))
    {
        int saved_errno =
            errno;


        if (saved_errno == 0)
        {
            saved_errno =
                ENOTDIR;
        }


        close(directory_fd);

        errno =
            saved_errno;


        return -1;
    }


    return directory_fd;
}


static int parse_upload_metadata(
    const char *metadata,
    char *filename,
    size_t filename_size,
    uint64_t *file_size
)
{
    if (metadata == NULL ||
        filename == NULL ||
        filename_size == 0 ||
        file_size == NULL)
    {
        return -1;
    }


    const char *separator =
        strchr(
            metadata,
            '|'
        );


    if (separator == NULL ||
        strchr(
            separator + 1,
            '|'
        ) != NULL)
    {
        return -1;
    }


    size_t name_length =
        (size_t)(
            separator -
            metadata
        );


    if (name_length == 0 ||
        name_length >=
            filename_size)
    {
        return -1;
    }


    memcpy(
        filename,
        metadata,
        name_length
    );


    filename[
        name_length
    ] = '\0';


    if (!is_valid_shared_filename(
            filename
        ))
    {
        return -1;
    }


    const char *size_text =
        separator + 1;


    if (*size_text == '\0')
    {
        return -1;
    }


    errno = 0;


    char *end_pointer = NULL;


    unsigned long long parsed =
        strtoull(
            size_text,
            &end_pointer,
            10
        );


    if (errno != 0 ||
        end_pointer == size_text ||
        *end_pointer != '\0' ||
        parsed > FILE_MAX_SIZE)
    {
        return -1;
    }


    *file_size =
        (uint64_t)parsed;


    return 0;
}


static int build_upload_temp_name(
    int socket_fd,
    char *buffer,
    size_t buffer_size
)
{
    int written =
        snprintf(
            buffer,
            buffer_size,
            ".upload-%ld-%d-%lu.tmp",
            (long)getpid(),
            socket_fd,
            (unsigned long)pthread_self()
        );


    if (written < 0 ||
        (size_t)written >=
            buffer_size)
    {
        errno = ENAMETOOLONG;

        return -1;
    }


    return 0;
}


int file_manager_handle_upload(
    int socket_fd,
    const char *username,
    const char *metadata
)
{
    char filename[
        FILE_NAME_MAX_SIZE
    ];

    uint64_t file_size;


    if (username == NULL)
    {
        return -1;
    }


    if (parse_upload_metadata(
            metadata,
            filename,
            sizeof(filename),
            &file_size
        ) != 0)
    {
        logger_client_log(
            LOG_WARN,
            "SECURITY",
            "Rejected upload metadata username=%s",
            username
        );


        return send_error(
            socket_fd,
            "INVALID_UPLOAD_METADATA"
        );
    }


    if (ensure_storage_directory() != 0)
    {
        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Storage directory unavailable during upload error=%s",
            strerror(errno)
        );


        return send_error(
            socket_fd,
            "STORAGE_UNAVAILABLE"
        );
    }


    int lock_fd =
        file_lock_acquire(
            filename,
            FILE_LOCK_EXCLUSIVE
        );


    if (lock_fd < 0)
    {
        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Unable to acquire upload lock username=%s filename=%s error=%s",
            username,
            filename,
            strerror(errno)
        );


        return send_error(
            socket_fd,
            "FILE_LOCK_FAILED"
        );
    }


    int result = 0;

    int storage_fd = -1;
    int file_fd = -1;

    int temp_created = 0;


    char temporary_name[
        FILE_NAME_MAX_SIZE
    ];


    storage_fd =
        open_storage_directory();


    if (storage_fd < 0)
    {
        logger_client_log(
            LOG_ERROR,
            "SECURITY",
            "Secure storage open failed during upload error=%s",
            strerror(errno)
        );


        result =
            send_error(
                socket_fd,
                "STORAGE_UNAVAILABLE"
            );


        goto upload_cleanup;
    }


    /*
     * Check the destination without following a
     * symbolic link.
     */
    struct stat destination_information;


    if (fstatat(
            storage_fd,
            filename,
            &destination_information,
            AT_SYMLINK_NOFOLLOW
        ) == 0)
    {
        result =
            send_error(
                socket_fd,
                "FILE_ALREADY_EXISTS"
            );


        goto upload_cleanup;
    }


    if (errno != ENOENT)
    {
        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Upload destination check failed filename=%s error=%s",
            filename,
            strerror(errno)
        );


        result =
            send_error(
                socket_fd,
                "UPLOAD_DESTINATION_CHECK_FAILED"
            );


        goto upload_cleanup;
    }


    if (build_upload_temp_name(
            socket_fd,
            temporary_name,
            sizeof(temporary_name)
        ) != 0)
    {
        result =
            send_error(
                socket_fd,
                "UPLOAD_TEMP_PATH_FAILED"
            );


        goto upload_cleanup;
    }


    /*
     * Remove only the exact internal temporary name
     * inside the already-open storage directory.
     */
    if (unlinkat(
            storage_fd,
            temporary_name,
            0
        ) != 0 &&
        errno != ENOENT)
    {
        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Unable to clear stale upload temp filename=%s error=%s",
            filename,
            strerror(errno)
        );


        result =
            send_error(
                socket_fd,
                "UPLOAD_TEMP_CLEANUP_FAILED"
            );


        goto upload_cleanup;
    }


    int open_flags =
        O_WRONLY |
        O_CREAT |
        O_EXCL |
        O_CLOEXEC;


#ifdef O_NOFOLLOW
    open_flags |=
        O_NOFOLLOW;
#endif


    file_fd =
        openat(
            storage_fd,
            temporary_name,
            open_flags,
            0600
        );


    if (file_fd < 0)
    {
        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Failed to create upload temp filename=%s error=%s",
            filename,
            strerror(errno)
        );


        result =
            send_error(
                socket_fd,
                "UPLOAD_CREATE_FAILED"
            );


        goto upload_cleanup;
    }


    temp_created = 1;


    /*
     * Raw bytes are accepted only after the server has
     * successfully validated metadata, locked the name,
     * opened storage safely, and created the temp file.
     */
    if (client_manager_send(
            socket_fd,
            MSG_RESPONSE,
            "UPLOAD_READY"
        ) != 0)
    {
        result = -1;

        goto upload_cleanup;
    }


    unsigned char buffer[
        FILE_CHUNK_SIZE
    ];


    uint64_t remaining =
        file_size;

    int write_failed = 0;


    while (remaining > 0)
    {
        size_t chunk_size =
            remaining > FILE_CHUNK_SIZE
                ? FILE_CHUNK_SIZE
                : (size_t)remaining;


        int receive_result =
            recv_all(
                socket_fd,
                buffer,
                chunk_size
            );


        if (receive_result != 0)
        {
            /*
             * The promised byte sequence was not
             * completed. The TCP application stream can
             * no longer be trusted, so the connection is
             * terminated by the caller.
             */
            logger_client_log(
                LOG_WARN,
                "FILE_MANAGER",
                "Upload interrupted username=%s filename=%s remaining=%" PRIu64,
                username,
                filename,
                remaining
            );


            result = -1;

            goto upload_cleanup;
        }


        /*
         * A local disk failure does NOT immediately stop
         * socket reception. Drain all promised bytes so
         * the next application frame remains aligned.
         */
        if (!write_failed &&
            write_all_file(
                file_fd,
                buffer,
                chunk_size
            ) != 0)
        {
            write_failed = 1;


            logger_client_log(
                LOG_ERROR,
                "FILE_MANAGER",
                "Upload write failed username=%s filename=%s error=%s",
                username,
                filename,
                strerror(errno)
            );
        }


        remaining -=
            (uint64_t)chunk_size;
    }


    if (!write_failed &&
        fsync(
            file_fd
        ) != 0)
    {
        write_failed = 1;
    }


    if (!write_failed &&
        fchmod(
            file_fd,
            0644
        ) != 0)
    {
        write_failed = 1;
    }


    if (close(
            file_fd
        ) != 0)
    {
        write_failed = 1;
    }


    file_fd = -1;


    if (write_failed)
    {
        result =
            send_error(
                socket_fd,
                "UPLOAD_WRITE_FAILED"
            );


        goto upload_cleanup;
    }


    /*
     * Publish atomically inside the same storage
     * directory and never overwrite an existing entry.
     */
    if (linkat(
            storage_fd,
            temporary_name,
            storage_fd,
            filename,
            0
        ) != 0)
    {
        int saved_errno =
            errno;


        if (saved_errno == EEXIST)
        {
            result =
                send_error(
                    socket_fd,
                    "FILE_ALREADY_EXISTS"
                );


            goto upload_cleanup;
        }


        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Upload finalize failed filename=%s error=%s",
            filename,
            strerror(saved_errno)
        );


        result =
            send_error(
                socket_fd,
                "UPLOAD_FINALIZE_FAILED"
            );


        goto upload_cleanup;
    }


    /*
     * The completed file is now published. Remove the
     * hidden temporary hard-link name.
     */
    if (unlinkat(
            storage_fd,
            temporary_name,
            0
        ) == 0)
    {
        temp_created = 0;
    }
    else
    {
        logger_client_log(
            LOG_WARN,
            "FILE_MANAGER",
            "Unable to remove upload temp link filename=%s error=%s",
            filename,
            strerror(errno)
        );
    }


    char response[
        FILE_NAME_MAX_SIZE + 64
    ];


    int response_written =
        snprintf(
            response,
            sizeof(response),
            "UPLOAD_SUCCESS %s %" PRIu64,
            filename,
            file_size
        );


    if (response_written < 0 ||
        (size_t)response_written >=
            sizeof(response))
    {
        result = -1;

        goto upload_cleanup;
    }


    if (client_manager_send(
            socket_fd,
            MSG_RESPONSE,
            response
        ) != 0)
    {
        logger_client_log(
            LOG_WARN,
            "FILE_MANAGER",
            "Upload stored but success response failed username=%s filename=%s",
            username,
            filename
        );


        result = -1;

        goto upload_cleanup;
    }


    logger_client_log(
        LOG_INFO,
        "FILE_MANAGER",
        "Upload success username=%s filename=%s size=%" PRIu64,
        username,
        filename,
        file_size
    );


upload_cleanup:

    if (file_fd >= 0)
    {
        if (close(
                file_fd
            ) != 0)
        {
            logger_client_log(
                LOG_WARN,
                "FILE_MANAGER",
                "Upload temp close failed filename=%s",
                filename
            );
        }
    }


    if (temp_created &&
        storage_fd >= 0)
    {
        if (unlinkat(
                storage_fd,
                temporary_name,
                0
            ) != 0 &&
            errno != ENOENT)
        {
            logger_client_log(
                LOG_WARN,
                "FILE_MANAGER",
                "Upload temp cleanup failed filename=%s error=%s",
                filename,
                strerror(errno)
            );
        }
    }


    if (storage_fd >= 0)
    {
        if (close(
                storage_fd
            ) != 0)
        {
            logger_client_log(
                LOG_WARN,
                "FILE_MANAGER",
                "Storage directory close failed after upload"
            );
        }
    }


    if (file_lock_release(
            lock_fd
        ) != 0)
    {
        logger_client_log(
            LOG_WARN,
            "FILE_MANAGER",
            "Unable to release upload lock username=%s filename=%s",
            username,
            filename
        );
    }


    return result;
}


int file_manager_handle_download(
    int socket_fd,
    const char *username,
    const char *filename
)
{
    if (username == NULL)
    {
        return -1;
    }


    if (filename == NULL ||
        !is_valid_shared_filename(
            filename
        ))
    {
        logger_client_log(
            LOG_WARN,
            "SECURITY",
            "Rejected download filename username=%s",
            username
        );


        return send_error(
            socket_fd,
            "INVALID_DOWNLOAD_FILENAME"
        );
    }


    if (ensure_storage_directory() != 0)
    {
        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Storage directory unavailable during download error=%s",
            strerror(errno)
        );


        return send_error(
            socket_fd,
            "STORAGE_UNAVAILABLE"
        );
    }


    int lock_fd =
        file_lock_acquire(
            filename,
            FILE_LOCK_SHARED
        );


    if (lock_fd < 0)
    {
        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Unable to acquire download lock username=%s filename=%s error=%s",
            username,
            filename,
            strerror(errno)
        );


        return send_error(
            socket_fd,
            "FILE_LOCK_FAILED"
        );
    }


    int result = 0;

    int storage_fd = -1;
    int file_fd = -1;


    storage_fd =
        open_storage_directory();


    if (storage_fd < 0)
    {
        logger_client_log(
            LOG_ERROR,
            "SECURITY",
            "Secure storage open failed during download error=%s",
            strerror(errno)
        );


        result =
            send_error(
                socket_fd,
                "STORAGE_UNAVAILABLE"
            );


        goto download_cleanup;
    }


    int open_flags =
        O_RDONLY |
        O_CLOEXEC;


#ifdef O_NOFOLLOW
    open_flags |=
        O_NOFOLLOW;
#endif


    /*
     * openat() confines lookup to the already-open
     * storage directory. O_NOFOLLOW blocks a final
     * symbolic-link entry.
     */
    file_fd =
        openat(
            storage_fd,
            filename,
            open_flags
        );


    if (file_fd < 0)
    {
        int saved_errno =
            errno;


        if (saved_errno == ENOENT)
        {
            result =
                send_error(
                    socket_fd,
                    "FILE_NOT_FOUND"
                );
        }
#ifdef ELOOP
        else if (saved_errno == ELOOP)
        {
            logger_client_log(
                LOG_WARN,
                "SECURITY",
                "Rejected symbolic-link download username=%s filename=%s",
                username,
                filename
            );


            result =
                send_error(
                    socket_fd,
                    "UNSAFE_FILE_ENTRY"
                );
        }
#endif
        else
        {
            logger_client_log(
                LOG_WARN,
                "FILE_MANAGER",
                "Download open failed username=%s filename=%s error=%s",
                username,
                filename,
                strerror(saved_errno)
            );


            result =
                send_error(
                    socket_fd,
                    "DOWNLOAD_OPEN_FAILED"
                );
        }


        goto download_cleanup;
    }


    struct stat information;


    if (fstat(
            file_fd,
            &information
        ) != 0)
    {
        result =
            send_error(
                socket_fd,
                "DOWNLOAD_STAT_FAILED"
            );


        goto download_cleanup;
    }


    if (!S_ISREG(
            information.st_mode
        ))
    {
        logger_client_log(
            LOG_WARN,
            "SECURITY",
            "Rejected non-regular download username=%s filename=%s",
            username,
            filename
        );


        result =
            send_error(
                socket_fd,
                "NOT_A_REGULAR_FILE"
            );


        goto download_cleanup;
    }


    if (information.st_size < 0)
    {
        result =
            send_error(
                socket_fd,
                "INVALID_FILE_SIZE"
            );


        goto download_cleanup;
    }


    uint64_t file_size =
        (uint64_t)information.st_size;


    if (file_size >
        FILE_MAX_SIZE)
    {
        result =
            send_error(
                socket_fd,
                "FILE_TOO_LARGE"
            );


        goto download_cleanup;
    }


    char response[
        MESSAGE_MAX_SIZE + 1
    ];


    int response_written =
        snprintf(
            response,
            sizeof(response),
            "DOWNLOAD_READY %s|%" PRIu64,
            filename,
            file_size
        );


    if (response_written < 0 ||
        (size_t)response_written >=
            sizeof(response))
    {
        result =
            send_error(
                socket_fd,
                "DOWNLOAD_METADATA_FAILED"
            );


        goto download_cleanup;
    }


    /*
     * client_manager_send_file_stream() holds the
     * destination client's send mutex for the metadata
     * frame plus every promised raw byte.
     */
    if (client_manager_send_file_stream(
            socket_fd,
            MSG_RESPONSE,
            response,
            file_fd,
            file_size
        ) != 0)
    {
        logger_client_log(
            LOG_WARN,
            "FILE_MANAGER",
            "Download stream failed username=%s filename=%s",
            username,
            filename
        );


        /*
         * A partial raw stream is not recoverable as an
         * application-frame stream. Force connection
         * teardown in the handler.
         */
        result = -1;

        goto download_cleanup;
    }


    logger_client_log(
        LOG_INFO,
        "FILE_MANAGER",
        "Download success username=%s filename=%s size=%" PRIu64,
        username,
        filename,
        file_size
    );


download_cleanup:

    if (file_fd >= 0)
    {
        if (close(
                file_fd
            ) != 0)
        {
            logger_client_log(
                LOG_WARN,
                "FILE_MANAGER",
                "Failed closing download source filename=%s",
                filename
            );
        }
    }


    if (storage_fd >= 0)
    {
        if (close(
                storage_fd
            ) != 0)
        {
            logger_client_log(
                LOG_WARN,
                "FILE_MANAGER",
                "Storage directory close failed after download"
            );
        }
    }


    if (file_lock_release(
            lock_fd
        ) != 0)
    {
        logger_client_log(
            LOG_WARN,
            "FILE_MANAGER",
            "Unable to release download lock username=%s filename=%s",
            username,
            filename
        );
    }


    return result;
}


int file_manager_handle_list(
    int socket_fd,
    const char *username
)
{
    if (username == NULL)
    {
        return -1;
    }


    if (ensure_storage_directory() != 0)
    {
        return send_error(
            socket_fd,
            "STORAGE_UNAVAILABLE"
        );
    }


    int directory_fd =
        open_storage_directory();


    if (directory_fd < 0)
    {
        logger_client_log(
            LOG_ERROR,
            "SECURITY",
            "Secure storage open failed during listing error=%s",
            strerror(errno)
        );


        return send_error(
            socket_fd,
            "FILE_LIST_OPEN_FAILED"
        );
    }


    DIR *directory =
        fdopendir(
            directory_fd
        );


    if (directory == NULL)
    {
        int saved_errno =
            errno;


        close(directory_fd);


        errno =
            saved_errno;


        return send_error(
            socket_fd,
            "FILE_LIST_OPEN_FAILED"
        );
    }


    /*
     * fdopendir() owns directory_fd from this point.
     */
    directory_fd = -1;


    if (client_manager_send(
            socket_fd,
            MSG_RESPONSE,
            "FILE_LIST_BEGIN"
        ) != 0)
    {
        closedir(directory);

        return -1;
    }


    int file_count = 0;
    int list_failed = 0;


    errno = 0;


    while (1)
    {
        struct dirent *entry =
            readdir(
                directory
            );


        if (entry == NULL)
        {
            if (errno != 0)
            {
                list_failed = 1;
            }


            break;
        }


        /*
         * This rejects internal/hidden names and every
         * filename outside SyncChat's basename policy.
         */
        if (!is_valid_shared_filename(
                entry->d_name
            ))
        {
            continue;
        }


        struct stat information;


        if (fstatat(
                dirfd(directory),
                entry->d_name,
                &information,
                AT_SYMLINK_NOFOLLOW
            ) != 0)
        {
            /*
             * The entry may have disappeared between
             * readdir() and fstatat(). Skip it rather
             * than failing the entire listing.
             */
            continue;
        }


        /*
         * Symbolic links, directories, sockets, FIFOs,
         * devices, and other non-regular entries are not
         * advertised to clients.
         */
        if (!S_ISREG(
                information.st_mode
            ) ||
            information.st_size < 0)
        {
            continue;
        }


        uint64_t file_size =
            (uint64_t)information.st_size;


        if (file_size >
            FILE_MAX_SIZE)
        {
            continue;
        }


        char file_entry[
            FILE_NAME_MAX_SIZE + 64
        ];


        int written =
            snprintf(
                file_entry,
                sizeof(file_entry),
                "%s|%" PRIu64,
                entry->d_name,
                file_size
            );


        if (written < 0 ||
            (size_t)written >=
                sizeof(file_entry))
        {
            continue;
        }


        if (client_manager_send(
                socket_fd,
                MSG_FILELIST,
                file_entry
            ) != 0)
        {
            closedir(directory);

            return -1;
        }


        file_count++;
    }


    if (closedir(
            directory
        ) != 0)
    {
        list_failed = 1;
    }


    if (list_failed)
    {
        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Shared file listing failed requester=%s",
            username
        );


        return send_error(
            socket_fd,
            "FILE_LIST_READ_FAILED"
        );
    }


    char end_message[64];


    int end_written =
        snprintf(
            end_message,
            sizeof(end_message),
            "FILE_LIST_END %d",
            file_count
        );


    if (end_written < 0 ||
        (size_t)end_written >=
            sizeof(end_message))
    {
        return -1;
    }


    if (client_manager_send(
            socket_fd,
            MSG_RESPONSE,
            end_message
        ) != 0)
    {
        return -1;
    }


    logger_client_log(
        LOG_DEBUG,
        "FILE_MANAGER",
        "Shared file list requested username=%s count=%d",
        username,
        file_count
    );


    return 0;
}
