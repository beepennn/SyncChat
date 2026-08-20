#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "client/upload_client.h"
#include "common/file_transfer.h"
#include "common/network_io.h"
#include "common/protocol.h"


typedef enum
{
    UPLOAD_IDLE = 0,
    UPLOAD_WAIT_READY,
    UPLOAD_WAIT_RESULT

} upload_state_t;


static pthread_mutex_t upload_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t upload_condition =
    PTHREAD_COND_INITIALIZER;


static upload_state_t upload_state =
    UPLOAD_IDLE;


static int response_available = 0;

static int connection_closed = 0;

static uint32_t response_type = 0;

static char response_payload[
    MESSAGE_MAX_SIZE + 1
];


static int send_text_frame(
    int socket_fd,
    uint32_t type,
    const char *payload
)
{
    if (payload == NULL)
    {
        return -1;
    }

    size_t length =
        strlen(payload);

    if (length >
        MESSAGE_MAX_SIZE)
    {
        return -1;
    }

    message_header_t header;

    header.type =
        htonl(type);

    header.length =
        htonl(
            (uint32_t)length
        );

    if (send_all(
            socket_fd,
            &header,
            sizeof(header)
        ) != 0)
    {
        return -1;
    }

    if (length == 0)
    {
        return 0;
    }

    return send_all(
        socket_fd,
        payload,
        length
    );
}


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


static int wait_for_response(
    uint32_t *type,
    char *payload,
    size_t payload_size
)
{
    if (pthread_mutex_lock(
            &upload_mutex
        ) != 0)
    {
        return -1;
    }


    while (!response_available &&
           !connection_closed)
    {
        if (pthread_cond_wait(
                &upload_condition,
                &upload_mutex
            ) != 0)
        {
            pthread_mutex_unlock(
                &upload_mutex
            );

            return -1;
        }
    }


    if (connection_closed)
    {
        pthread_mutex_unlock(
            &upload_mutex
        );

        return -1;
    }


    *type =
        response_type;


    copy_result(
        payload,
        payload_size,
        response_payload
    );


    response_available = 0;


    pthread_mutex_unlock(
        &upload_mutex
    );


    return 0;
}


int upload_client_handle_server_message(
    uint32_t message_type,
    const char *payload
)
{
    if (payload == NULL)
    {
        return 0;
    }


    if (pthread_mutex_lock(
            &upload_mutex
        ) != 0)
    {
        return 0;
    }


    if (upload_state ==
        UPLOAD_IDLE)
    {
        pthread_mutex_unlock(
            &upload_mutex
        );

        return 0;
    }


    int is_upload_message = 0;


    if (message_type ==
        MSG_ERROR)
    {
        is_upload_message = 1;
    }
    else if (message_type ==
             MSG_RESPONSE)
    {
        if (strncmp(
                payload,
                "UPLOAD_",
                7
            ) == 0)
        {
            is_upload_message = 1;
        }
    }


    if (!is_upload_message)
    {
        pthread_mutex_unlock(
            &upload_mutex
        );

        return 0;
    }


    response_type =
        message_type;


    snprintf(
        response_payload,
        sizeof(response_payload),
        "%s",
        payload
    );


    response_available = 1;


    pthread_cond_signal(
        &upload_condition
    );


    pthread_mutex_unlock(
        &upload_mutex
    );


    return 1;
}


void upload_client_notify_connection_closed(void)
{
    if (pthread_mutex_lock(
            &upload_mutex
        ) != 0)
    {
        return;
    }


    connection_closed = 1;


    pthread_cond_broadcast(
        &upload_condition
    );


    pthread_mutex_unlock(
        &upload_mutex
    );
}


int upload_client_upload(
    int socket_fd,
    const char *local_path,
    char *result,
    size_t result_size
)
{
    if (local_path == NULL ||
        result == NULL ||
        result_size == 0)
    {
        return 1;
    }


    int file_fd = open(
        local_path,
        O_RDONLY
    );


    if (file_fd < 0)
    {
        copy_result(
            result,
            result_size,
            "LOCAL_FILE_OPEN_FAILED"
        );

        return 1;
    }


    struct stat information;


    if (fstat(
            file_fd,
            &information
        ) != 0)
    {
        close(file_fd);

        copy_result(
            result,
            result_size,
            "LOCAL_FILE_STAT_FAILED"
        );

        return 1;
    }


    if (!S_ISREG(
            information.st_mode
        ))
    {
        close(file_fd);

        copy_result(
            result,
            result_size,
            "NOT_A_REGULAR_FILE"
        );

        return 1;
    }


    if (information.st_size < 0)
    {
        close(file_fd);

        copy_result(
            result,
            result_size,
            "INVALID_FILE_SIZE"
        );

        return 1;
    }


    uint64_t file_size =
        (uint64_t)information.st_size;


    if (file_size >
        FILE_MAX_SIZE)
    {
        close(file_fd);

        copy_result(
            result,
            result_size,
            "FILE_TOO_LARGE"
        );

        return 1;
    }


    /*
     * Extract basename from local path.
     */
    const char *filename =
        strrchr(
            local_path,
            '/'
        );


    if (filename == NULL)
    {
        filename =
            local_path;
    }
    else
    {
        filename++;
    }


    if (!is_valid_shared_filename(
            filename
        ))
    {
        close(file_fd);

        copy_result(
            result,
            result_size,
            "INVALID_SHARED_FILENAME"
        );

        return 1;
    }


    char metadata[
        MESSAGE_MAX_SIZE + 1
    ];


    int metadata_written =
        snprintf(
            metadata,
            sizeof(metadata),
            "%s|%" PRIu64,
            filename,
            file_size
        );


    if (metadata_written < 0 ||
        (size_t)metadata_written >=
            sizeof(metadata))
    {
        close(file_fd);

        copy_result(
            result,
            result_size,
            "UPLOAD_METADATA_FAILED"
        );

        return 1;
    }


    if (pthread_mutex_lock(
            &upload_mutex
        ) != 0)
    {
        close(file_fd);

        return -1;
    }


    if (upload_state !=
        UPLOAD_IDLE)
    {
        pthread_mutex_unlock(
            &upload_mutex
        );

        close(file_fd);

        copy_result(
            result,
            result_size,
            "UPLOAD_ALREADY_ACTIVE"
        );

        return 1;
    }


    upload_state =
        UPLOAD_WAIT_READY;

    response_available = 0;

    connection_closed = 0;


    pthread_mutex_unlock(
        &upload_mutex
    );


    /*
     * Send upload metadata.
     */
    if (send_text_frame(
            socket_fd,
            MSG_UPLOAD,
            metadata
        ) != 0)
    {
        close(file_fd);

        shutdown(
            socket_fd,
            SHUT_RDWR
        );

        return -1;
    }


    uint32_t server_type;

    char server_response[
        MESSAGE_MAX_SIZE + 1
    ];


    if (wait_for_response(
            &server_type,
            server_response,
            sizeof(server_response)
        ) != 0)
    {
        close(file_fd);

        return -1;
    }


    if (server_type !=
            MSG_RESPONSE ||
        strcmp(
            server_response,
            "UPLOAD_READY"
        ) != 0)
    {
        pthread_mutex_lock(
            &upload_mutex
        );

        upload_state =
            UPLOAD_IDLE;

        pthread_mutex_unlock(
            &upload_mutex
        );


        close(file_fd);


        copy_result(
            result,
            result_size,
            server_response
        );


        return 1;
    }


    /*
     * Prepare for final server response before
     * transmitting data.
     */
    pthread_mutex_lock(
        &upload_mutex
    );

    upload_state =
        UPLOAD_WAIT_RESULT;

    response_available = 0;

    pthread_mutex_unlock(
        &upload_mutex
    );


    unsigned char buffer[
        FILE_CHUNK_SIZE
    ];


    uint64_t remaining =
        file_size;


    while (remaining > 0)
    {
        size_t requested;


        if (remaining >
            FILE_CHUNK_SIZE)
        {
            requested =
                FILE_CHUNK_SIZE;
        }
        else
        {
            requested =
                (size_t)remaining;
        }


        ssize_t bytes_read =
            read(
                file_fd,
                buffer,
                requested
            );


        if (bytes_read < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            close(file_fd);

            shutdown(
                socket_fd,
                SHUT_RDWR
            );

            copy_result(
                result,
                result_size,
                "LOCAL_FILE_READ_FAILED"
            );

            return -1;
        }


        if (bytes_read == 0)
        {
            close(file_fd);

            shutdown(
                socket_fd,
                SHUT_RDWR
            );

            copy_result(
                result,
                result_size,
                "LOCAL_FILE_CHANGED_DURING_UPLOAD"
            );

            return -1;
        }


        if (send_all(
                socket_fd,
                buffer,
                (size_t)bytes_read
            ) != 0)
        {
            close(file_fd);

            shutdown(
                socket_fd,
                SHUT_RDWR
            );

            copy_result(
                result,
                result_size,
                "UPLOAD_SEND_FAILED"
            );

            return -1;
        }


        remaining -=
            (uint64_t)bytes_read;
    }


    close(file_fd);


    if (wait_for_response(
            &server_type,
            server_response,
            sizeof(server_response)
        ) != 0)
    {
        return -1;
    }


    pthread_mutex_lock(
        &upload_mutex
    );

    upload_state =
        UPLOAD_IDLE;

    pthread_mutex_unlock(
        &upload_mutex
    );


    copy_result(
        result,
        result_size,
        server_response
    );


    if (server_type ==
            MSG_RESPONSE &&
        strncmp(
            server_response,
            "UPLOAD_SUCCESS",
            strlen("UPLOAD_SUCCESS")
        ) == 0)
    {
        return 0;
    }


    return 1;
}
