#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "client/download_client.h"
#include "common/file_transfer.h"
#include "common/network_io.h"
#include "common/protocol.h"


static void copy_result(
    char *destination,
    size_t destination_size,
    const char *message
)
{
    if (destination == NULL ||
        destination_size == 0)
    {
        return;
    }


    if (message == NULL)
    {
        destination[0] = '\0';

        return;
    }


    snprintf(
        destination,
        destination_size,
        "%s",
        message
    );
}


static int write_all_file(
    int file_fd,
    const void *buffer,
    size_t length
)
{
    const unsigned char *bytes =
        (const unsigned char *)buffer;


    size_t total_written = 0;


    while (total_written <
           length)
    {
        ssize_t written =
            write(
                file_fd,
                bytes + total_written,
                length - total_written
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


        total_written +=
            (size_t)written;
    }


    return 0;
}


static int ensure_download_directory(void)
{
    struct stat information;


    if (stat(
            FILE_DOWNLOAD_DIRECTORY,
            &information
        ) == 0)
    {
        return S_ISDIR(
            information.st_mode
        )
            ? 0
            : -1;
    }


    if (errno != ENOENT)
    {
        return -1;
    }


    if (mkdir(
            FILE_DOWNLOAD_DIRECTORY,
            0755
        ) != 0 &&
        errno != EEXIST)
    {
        return -1;
    }


    return 0;
}


static int parse_download_ready(
    const char *payload,
    char *filename,
    size_t filename_size,
    uint64_t *file_size
)
{
    static const char prefix[] =
        "DOWNLOAD_READY ";


    if (strncmp(
            payload,
            prefix,
            sizeof(prefix) - 1
        ) != 0)
    {
        return 0;
    }


    const char *metadata =
        payload +
        sizeof(prefix) - 1;


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


    size_t filename_length =
        (size_t)(
            separator -
            metadata
        );


    if (filename_length == 0 ||
        filename_length >=
            filename_size)
    {
        return -1;
    }


    memcpy(
        filename,
        metadata,
        filename_length
    );


    filename[
        filename_length
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


    return 1;
}


int download_client_handle_server_message(
    int socket_fd,
    uint32_t message_type,
    const char *payload,
    char *result,
    size_t result_size
)
{
    if (payload == NULL ||
        result == NULL ||
        result_size == 0)
    {
        return -1;
    }


    if (message_type !=
        MSG_RESPONSE)
    {
        return 0;
    }


    char filename[
        FILE_NAME_MAX_SIZE
    ];


    uint64_t file_size;


    int parse_result =
        parse_download_ready(
            payload,
            filename,
            sizeof(filename),
            &file_size
        );


    if (parse_result == 0)
    {
        return 0;
    }


    if (parse_result < 0)
    {
        copy_result(
            result,
            result_size,
            "INVALID_DOWNLOAD_METADATA"
        );


        return -1;
    }


    int local_write_failed = 0;


    if (ensure_download_directory() != 0)
    {
        local_write_failed = 1;
    }


    char final_path[
        PATH_MAX
    ];


    char temporary_path[
        PATH_MAX
    ];


    int final_written =
        snprintf(
            final_path,
            sizeof(final_path),
            "%s/%s",
            FILE_DOWNLOAD_DIRECTORY,
            filename
        );


    int temporary_written =
        snprintf(
            temporary_path,
            sizeof(temporary_path),
            "%s/.download-%ld.tmp",
            FILE_DOWNLOAD_DIRECTORY,
            (long)getpid()
        );


    if (final_written < 0 ||
        temporary_written < 0 ||
        (size_t)final_written >=
            sizeof(final_path) ||
        (size_t)temporary_written >=
            sizeof(temporary_path))
    {
        local_write_failed = 1;
    }


    /*
     * Never overwrite an existing local download.
     */
    if (!local_write_failed &&
        access(
            final_path,
            F_OK
        ) == 0)
    {
        local_write_failed = 1;
    }


    int file_fd = -1;


    if (!local_write_failed)
    {
        unlink(
            temporary_path
        );


        file_fd =
            open(
                temporary_path,
                O_WRONLY |
                O_CREAT |
                O_EXCL,
                0600
            );


        if (file_fd < 0)
        {
            local_write_failed = 1;
        }
    }


    unsigned char buffer[
        FILE_CHUNK_SIZE
    ];


    uint64_t remaining =
        file_size;


    /*
     * IMPORTANT:
     *
     * Even if local disk writing fails, continue
     * receiving/draining every promised file byte.
     *
     * Otherwise the next bytes would be mistaken for
     * an application message header.
     */
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
            if (file_fd >= 0)
            {
                close(file_fd);
            }


            if (temporary_path[0] != '\0')
            {
                unlink(
                    temporary_path
                );
            }


            copy_result(
                result,
                result_size,
                "DOWNLOAD_CONNECTION_FAILED"
            );


            return -1;
        }


        if (!local_write_failed)
        {
            if (write_all_file(
                    file_fd,
                    buffer,
                    chunk_size
                ) != 0)
            {
                local_write_failed = 1;
            }
        }


        remaining -=
            (uint64_t)chunk_size;
    }


    if (file_fd >= 0)
    {
        if (!local_write_failed &&
            fsync(file_fd) != 0)
        {
            local_write_failed = 1;
        }


        if (!local_write_failed &&
            fchmod(
                file_fd,
                0644
            ) != 0)
        {
            local_write_failed = 1;
        }


        if (close(
                file_fd
            ) != 0)
        {
            local_write_failed = 1;
        }
    }


    if (local_write_failed)
    {
        unlink(
            temporary_path
        );


        copy_result(
            result,
            result_size,
            "LOCAL_DOWNLOAD_WRITE_FAILED_OR_FILE_EXISTS"
        );


        return 2;
    }


    /*
     * Publish the completely downloaded file without
     * overwriting an existing local file.
     */
    if (link(
            temporary_path,
            final_path
        ) != 0)
    {
        unlink(
            temporary_path
        );


        copy_result(
            result,
            result_size,
            "LOCAL_DOWNLOAD_FINALIZE_FAILED"
        );


        return 2;
    }


    unlink(
        temporary_path
    );


    int written =
        snprintf(
            result,
            result_size,
            "DOWNLOAD_SUCCESS %s %" PRIu64,
            filename,
            file_size
        );


    if (written < 0 ||
        (size_t)written >=
            result_size)
    {
        return 2;
    }


    return 1;
}
