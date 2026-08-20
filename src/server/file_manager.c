#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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
        ssize_t written = write(
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
            return -1;
        }

        written_total +=
            (size_t)written;
    }

    return 0;
}


static int ensure_storage_directory(void)
{
    struct stat information;

    if (stat(
            FILE_STORAGE_DIRECTORY,
            &information
        ) == 0)
    {
        if (!S_ISDIR(
                information.st_mode
            ))
        {
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
        ) != 0)
    {
        if (errno != EEXIST)
        {
            return -1;
        }
    }

    return 0;
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
        strchr(metadata, '|');

    if (separator == NULL)
    {
        return -1;
    }

    /*
     * Only one separator is allowed.
     */
    if (strchr(
            separator + 1,
            '|'
        ) != NULL)
    {
        return -1;
    }

    size_t name_length =
        (size_t)(
            separator - metadata
        );

    if (name_length == 0 ||
        name_length >= filename_size)
    {
        return -1;
    }

    memcpy(
        filename,
        metadata,
        name_length
    );

    filename[name_length] = '\0';

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
        *end_pointer != '\0')
    {
        return -1;
    }

    if (parsed > FILE_MAX_SIZE)
    {
        return -1;
    }

    *file_size =
        (uint64_t)parsed;

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


    if (parse_upload_metadata(
            metadata,
            filename,
            sizeof(filename),
            &file_size
        ) != 0)
    {
        client_manager_send(
            socket_fd,
            MSG_ERROR,
            "INVALID_UPLOAD_METADATA"
        );

        logger_client_log(
            LOG_WARN,
            "FILE_MANAGER",
            "Invalid upload metadata username=%s",
            username
        );

        return 0;
    }


    if (ensure_storage_directory() != 0)
    {
        client_manager_send(
            socket_fd,
            MSG_ERROR,
            "STORAGE_UNAVAILABLE"
        );

        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Storage directory unavailable"
        );

        return 0;
    }


    char final_path[PATH_MAX];

    int path_written =
        snprintf(
            final_path,
            sizeof(final_path),
            "%s/%s",
            FILE_STORAGE_DIRECTORY,
            filename
        );

    if (path_written < 0 ||
        (size_t)path_written >=
            sizeof(final_path))
    {
        client_manager_send(
            socket_fd,
            MSG_ERROR,
            "INVALID_FILE_PATH"
        );

        return 0;
    }


    /*
     * Fast duplicate check.
     *
     * The final atomic link() still protects
     * against races between simultaneous uploads.
     */
    if (access(
            final_path,
            F_OK
        ) == 0)
    {
        client_manager_send(
            socket_fd,
            MSG_ERROR,
            "FILE_ALREADY_EXISTS"
        );

        return 0;
    }


    /*
     * Upload to a hidden temporary file first.
     *
     * This prevents incomplete files from appearing
     * as normal shared files.
     */
    char temporary_path[
        PATH_MAX
    ];

    int temporary_written =
        snprintf(
            temporary_path,
            sizeof(temporary_path),
            "%s/.upload-%ld-%d-%lu.tmp",
            FILE_STORAGE_DIRECTORY,
            (long)getpid(),
            socket_fd,
            (unsigned long)pthread_self()
        );

    if (temporary_written < 0 ||
        (size_t)temporary_written >=
            sizeof(temporary_path))
    {
        client_manager_send(
            socket_fd,
            MSG_ERROR,
            "UPLOAD_TEMP_PATH_FAILED"
        );

        return 0;
    }


    /*
     * Remove a stale temporary file from an
     * abnormal previous termination if necessary.
     */
    unlink(temporary_path);


    int file_fd = open(
        temporary_path,
        O_WRONLY |
        O_CREAT |
        O_EXCL,
        0600
    );

    if (file_fd < 0)
    {
        client_manager_send(
            socket_fd,
            MSG_ERROR,
            "UPLOAD_CREATE_FAILED"
        );

        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Failed to create upload temp file filename=%s error=%s",
            filename,
            strerror(errno)
        );

        return 0;
    }


    /*
     * The server has accepted the metadata.
     * Only now may the client begin sending raw
     * binary file blocks.
     */
    if (client_manager_send(
            socket_fd,
            MSG_RESPONSE,
            "UPLOAD_READY"
        ) != 0)
    {
        close(file_fd);
        unlink(temporary_path);

        return -1;
    }


    unsigned char buffer[
        FILE_CHUNK_SIZE
    ];

    uint64_t remaining =
        file_size;

    int write_failed = 0;


    while (remaining > 0)
    {
        size_t chunk_size;

        if (remaining >
            FILE_CHUNK_SIZE)
        {
            chunk_size =
                FILE_CHUNK_SIZE;
        }
        else
        {
            chunk_size =
                (size_t)remaining;
        }


        int receive_result =
            recv_all(
                socket_fd,
                buffer,
                chunk_size
            );

        if (receive_result != 0)
        {
            close(file_fd);
            unlink(temporary_path);

            logger_client_log(
                LOG_WARN,
                "FILE_MANAGER",
                "Upload interrupted username=%s filename=%s",
                username,
                filename
            );

            return -1;
        }


        /*
         * If the local write fails, continue reading
         * the remaining bytes from TCP so application
         * framing remains synchronized.
         */
        if (!write_failed)
        {
            if (write_all_file(
                    file_fd,
                    buffer,
                    chunk_size
                ) != 0)
            {
                write_failed = 1;
            }
        }


        remaining -=
            (uint64_t)chunk_size;
    }


    if (!write_failed)
    {
        if (fsync(file_fd) != 0)
        {
            write_failed = 1;
        }
    }


    if (!write_failed)
    {
        if (fchmod(
                file_fd,
                0644
            ) != 0)
        {
            write_failed = 1;
        }
    }


    if (close(file_fd) != 0)
    {
        write_failed = 1;
    }


    if (write_failed)
    {
        unlink(temporary_path);

        client_manager_send(
            socket_fd,
            MSG_ERROR,
            "UPLOAD_WRITE_FAILED"
        );

        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Upload write failed username=%s filename=%s",
            username,
            filename
        );

        return 0;
    }


    /*
     * link() publishes the completed file without
     * overwriting an existing destination.
     *
     * This is atomic within the same filesystem.
     */
    if (link(
            temporary_path,
            final_path
        ) != 0)
    {
        int saved_errno =
            errno;

        unlink(
            temporary_path
        );

        if (saved_errno == EEXIST)
        {
            client_manager_send(
                socket_fd,
                MSG_ERROR,
                "FILE_ALREADY_EXISTS"
            );

            return 0;
        }


        client_manager_send(
            socket_fd,
            MSG_ERROR,
            "UPLOAD_FINALIZE_FAILED"
        );

        logger_client_log(
            LOG_ERROR,
            "FILE_MANAGER",
            "Upload finalize failed filename=%s error=%s",
            filename,
            strerror(saved_errno)
        );

        return 0;
    }


    /*
     * Remove the temporary hard-link name.
     * The final filename remains.
     */
    if (unlink(
            temporary_path
        ) != 0)
    {
        logger_client_log(
            LOG_WARN,
            "FILE_MANAGER",
            "Unable to remove temporary upload path filename=%s",
            filename
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
        return -1;
    }


    client_manager_send(
        socket_fd,
        MSG_RESPONSE,
        response
    );


    logger_client_log(
        LOG_INFO,
        "FILE_MANAGER",
        "Upload success username=%s filename=%s size=%" PRIu64,
        username,
        filename,
        file_size
    );


    return 0;
}
