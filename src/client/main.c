#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client/download_client.h"
#include "client/upload_client.h"
#include "common/file_transfer.h"
#include "common/network_io.h"
#include "common/protocol.h"
#include "server/server_config.h"


static int client_socket = -1;

static atomic_int client_running = 1;

static pthread_mutex_t output_mutex =
    PTHREAD_MUTEX_INITIALIZER;


/*
 * Thread-safe console output.
 */
static void safe_printf(
    const char *prefix,
    const char *message
)
{
    if (pthread_mutex_lock(
            &output_mutex
        ) != 0)
    {
        return;
    }

    if (prefix != NULL)
    {
        printf(
            "%s",
            prefix
        );
    }

    if (message != NULL)
    {
        printf(
            "%s",
            message
        );
    }

    fflush(stdout);

    pthread_mutex_unlock(
        &output_mutex
    );
}


/*
 * Send one complete framed application message.
 *
 * After login, the main/input thread performs
 * normal client-side message transmission.
 */
static int send_message(
    uint32_t type,
    const char *payload
)
{
    message_header_t header;

    size_t payload_length;


    if (payload == NULL)
    {
        return -1;
    }


    if (!is_valid_message_type(type))
    {
        return -1;
    }


    payload_length =
        strlen(payload);


    if (payload_length >
        MESSAGE_MAX_SIZE)
    {
        return -1;
    }


    header.type =
        htonl(type);


    header.length =
        htonl(
            (uint32_t)payload_length
        );


    if (send_all(
            client_socket,
            &header,
            sizeof(header)
        ) != 0)
    {
        return -1;
    }


    if (payload_length == 0)
    {
        return 0;
    }


    if (send_all(
            client_socket,
            payload,
            payload_length
        ) != 0)
    {
        return -1;
    }


    return 0;
}


/*
 * Receive one complete framed message.
 *
 * Return:
 *
 *   0  message received
 *   1  server closed connection
 *  -1  socket/protocol error
 */
static int receive_message(
    uint32_t *message_type,
    char *payload,
    size_t payload_size
)
{
    message_header_t header;

    uint32_t type;
    uint32_t length;

    int result;


    if (message_type == NULL ||
        payload == NULL ||
        payload_size == 0)
    {
        return -1;
    }


    result = recv_all(
        client_socket,
        &header,
        sizeof(header)
    );


    if (result != 0)
    {
        return result;
    }


    type =
        ntohl(header.type);

    length =
        ntohl(header.length);


    if (!is_valid_message_type(type))
    {
        fprintf(
            stderr,
            "Invalid message type received: %u\n",
            type
        );

        return -1;
    }


    if (length >
        MESSAGE_MAX_SIZE)
    {
        fprintf(
            stderr,
            "Received message exceeds maximum size.\n"
        );

        return -1;
    }


    if ((size_t)length >=
        payload_size)
    {
        fprintf(
            stderr,
            "Receive buffer is too small.\n"
        );

        return -1;
    }


    if (length > 0)
    {
        result = recv_all(
            client_socket,
            payload,
            length
        );


        if (result != 0)
        {
            return result;
        }
    }


    payload[length] = '\0';

    *message_type = type;


    return 0;
}


/*
 * Establish the TCP connection to SyncChat.
 */
static int connect_to_server(void)
{
    struct sockaddr_in server_address;


    client_socket = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );


    if (client_socket < 0)
    {
        perror("socket");

        return -1;
    }


    memset(
        &server_address,
        0,
        sizeof(server_address)
    );


    server_address.sin_family =
        AF_INET;

    server_address.sin_port =
        htons(SERVER_PORT);


    if (inet_pton(
            AF_INET,
            "127.0.0.1",
            &server_address.sin_addr
        ) != 1)
    {
        fprintf(
            stderr,
            "Invalid server address.\n"
        );


        close(client_socket);

        client_socket = -1;


        return -1;
    }


    if (connect(
            client_socket,
            (struct sockaddr *)&server_address,
            sizeof(server_address)
        ) < 0)
    {
        perror("connect");


        close(client_socket);

        client_socket = -1;


        return -1;
    }


    return 0;
}


/*
 * Close client socket exactly once.
 */
static void cleanup_socket(void)
{
    if (client_socket >= 0)
    {
        close(
            client_socket
        );

        client_socket = -1;
    }
}


/*
 * Dedicated asynchronous receiver thread.
 *
 * After login this is the only thread that receives
 * data from the TCP connection.
 *
 * File-download raw bytes are also consumed here.
 */
static void *receiver_thread(
    void *argument
)
{
    (void)argument;


    char payload[
        MESSAGE_MAX_SIZE + 1
    ];


    char download_result[
        MESSAGE_MAX_SIZE + 1
    ];


    uint32_t message_type;


    while (atomic_load(
            &client_running
        ))
    {
        int result =
            receive_message(
                &message_type,
                payload,
                sizeof(payload)
            );


        /*
         * Server performed an orderly shutdown.
         */
        if (result == 1)
        {
            /*
             * Wake an upload operation that might be
             * waiting on its condition variable.
             */
            upload_client_notify_connection_closed();


            safe_printf(
                "\n",
                "Server closed the connection.\n"
            );


            atomic_store(
                &client_running,
                0
            );


            break;
        }


        /*
         * Socket or protocol error.
         */
        if (result < 0)
        {
            upload_client_notify_connection_closed();


            if (atomic_load(
                    &client_running
                ))
            {
                safe_printf(
                    "\n",
                    "Connection receive error.\n"
                );
            }


            atomic_store(
                &client_running,
                0
            );


            break;
        }


        /*
         * ------------------------------------------------
         * UPLOAD CONTROL RESPONSES
         * ------------------------------------------------
         *
         * The receiver thread receives UPLOAD_READY and
         * UPLOAD_SUCCESS/UPLOAD_ERROR responses.
         *
         * upload_client.c passes them to the input thread
         * through a mutex + condition variable.
         */
        if (upload_client_handle_server_message(
                message_type,
                payload
            ))
        {
            continue;
        }


        /*
         * ------------------------------------------------
         * DOWNLOAD STREAM
         * ------------------------------------------------
         *
         * A DOWNLOAD_READY response is immediately
         * followed by exactly file_size raw bytes.
         *
         * The receiver thread therefore remains the
         * exclusive socket reader while reconstructing
         * the downloaded file.
         */
        int download_status =
            download_client_handle_server_message(
                client_socket,
                message_type,
                payload,
                download_result,
                sizeof(download_result)
            );


        if (download_status != 0)
        {
            /*
             * Successful download.
             */
            if (download_status == 1)
            {
                if (pthread_mutex_lock(
                        &output_mutex
                    ) == 0)
                {
                    printf(
                        "\n[Download] %s\n",
                        download_result
                    );


                    fflush(stdout);


                    pthread_mutex_unlock(
                        &output_mutex
                    );
                }
            }

            /*
             * Network stream remained valid but the
             * local file could not be stored.
             */
            else if (download_status == 2)
            {
                if (pthread_mutex_lock(
                        &output_mutex
                    ) == 0)
                {
                    printf(
                        "\n[Download Error] %s\n",
                        download_result
                    );


                    fflush(stdout);


                    pthread_mutex_unlock(
                        &output_mutex
                    );
                }
            }

            /*
             * The TCP stream became unusable.
             */
            else
            {
                upload_client_notify_connection_closed();


                atomic_store(
                    &client_running,
                    0
                );


                shutdown(
                    client_socket,
                    SHUT_RDWR
                );


                break;
            }


            continue;
        }


        /*
         * ------------------------------------------------
         * NORMAL SERVER MESSAGE
         * ------------------------------------------------
         */
        if (pthread_mutex_lock(
                &output_mutex
            ) != 0)
        {
            continue;
        }


        printf("\n");


        switch (message_type)
        {
            /*
             * Public chat.
             */
            case MSG_BROADCAST:
            {
                printf(
                    "%s\n",
                    payload
                );

                break;
            }


            /*
             * Private chat.
             */
            case MSG_PRIVATE:
            {
                printf(
                    "[Private] %s\n",
                    payload
                );

                break;
            }


            /*
             * General server response.
             */
            case MSG_RESPONSE:
            {
                printf(
                    "[Server] %s\n",
                    payload
                );


                if (strcmp(
                        payload,
                        "GOODBYE"
                    ) == 0)
                {
                    atomic_store(
                        &client_running,
                        0
                    );
                }


                break;
            }


            /*
             * Server error.
             */
            case MSG_ERROR:
            {
                printf(
                    "[Error] %s\n",
                    payload
                );

                break;
            }


            /*
             * Online user list.
             */
            case MSG_USERLIST:
            {
                printf(
                    "[Online Users]\n"
                    "%s\n",
                    payload
                );

                break;
            }


            default:
            {
                printf(
                    "[Server message type %u] %s\n",
                    message_type,
                    payload
                );

                break;
            }
        }


        fflush(stdout);


        pthread_mutex_unlock(
            &output_mutex
        );


        if (!atomic_load(
                &client_running
            ))
        {
            break;
        }
    }


    return NULL;
}


int main(void)
{
    char payload[
        MESSAGE_MAX_SIZE + 1
    ];


    char username[
        USERNAME_MAX_SIZE
    ];


    uint32_t message_type;


    pthread_t receiver;


    int receiver_created = 0;


    /*
     * ====================================================
     * CONNECT
     * ====================================================
     */
    if (connect_to_server() != 0)
    {
        return EXIT_FAILURE;
    }


    printf(
        "Connected to SyncChat server.\n"
    );


    /*
     * ====================================================
     * LOGIN PHASE
     *
     * Login remains synchronous.
     * ====================================================
     */
    int result =
        receive_message(
            &message_type,
            payload,
            sizeof(payload)
        );


    if (result != 0)
    {
        fprintf(
            stderr,
            "Failed to receive login request from server.\n"
        );


        cleanup_socket();


        return EXIT_FAILURE;
    }


    if (message_type !=
            MSG_RESPONSE ||
        strcmp(
            payload,
            "USERNAME_REQUIRED"
        ) != 0)
    {
        fprintf(
            stderr,
            "Unexpected initial server response: %s\n",
            payload
        );


        cleanup_socket();


        return EXIT_FAILURE;
    }


    /*
     * Read and validate username.
     */
    while (1)
    {
        printf(
            "Username: "
        );


        fflush(stdout);


        if (fgets(
                username,
                sizeof(username),
                stdin
            ) == NULL)
        {
            cleanup_socket();


            return EXIT_FAILURE;
        }


        username[
            strcspn(
                username,
                "\n"
            )
        ] = '\0';


        if (!is_valid_username(
                username
            ))
        {
            printf(
                "Invalid username. "
                "Use letters, digits, "
                "and underscore only.\n"
            );


            continue;
        }


        break;
    }


    /*
     * Send login request.
     */
    if (send_message(
            MSG_LOGIN,
            username
        ) != 0)
    {
        fprintf(
            stderr,
            "Failed to send login request.\n"
        );


        cleanup_socket();


        return EXIT_FAILURE;
    }


    /*
     * Wait synchronously for login result.
     */
    result =
        receive_message(
            &message_type,
            payload,
            sizeof(payload)
        );


    if (result != 0)
    {
        fprintf(
            stderr,
            "Failed to receive login response.\n"
        );


        cleanup_socket();


        return EXIT_FAILURE;
    }


    if (message_type ==
        MSG_ERROR)
    {
        printf(
            "Login rejected: %s\n",
            payload
        );


        cleanup_socket();


        return EXIT_FAILURE;
    }


    if (message_type !=
            MSG_RESPONSE ||
        strncmp(
            payload,
            "LOGIN_SUCCESS",
            strlen(
                "LOGIN_SUCCESS"
            )
        ) != 0)
    {
        fprintf(
            stderr,
            "Unexpected login response: %s\n",
            payload
        );


        cleanup_socket();


        return EXIT_FAILURE;
    }


    printf(
        "Login successful as '%s'.\n",
        username
    );


    printf(
        "Commands:\n"
        "  /users\n"
        "  /msg <user> <message>\n"
        "  /upload <local-path>\n"
        "  /download <filename>\n"
        "  /quit\n"
    );


    /*
     * ====================================================
     * ASYNCHRONOUS SESSION
     *
     * main thread:
     *     keyboard input + transmission
     *
     * receiver thread:
     *     server messages + download data
     * ====================================================
     */
    atomic_store(
        &client_running,
        1
    );


    int pthread_result =
        pthread_create(
            &receiver,
            NULL,
            receiver_thread,
            NULL
        );


    if (pthread_result != 0)
    {
        fprintf(
            stderr,
            "Failed to create receiver thread.\n"
        );


        cleanup_socket();


        return EXIT_FAILURE;
    }


    receiver_created = 1;


    /*
     * ====================================================
     * INPUT / SEND LOOP
     * ====================================================
     */
    while (atomic_load(
            &client_running
        ))
    {
        if (pthread_mutex_lock(
                &output_mutex
            ) == 0)
        {
            printf(
                "%s> ",
                username
            );


            fflush(stdout);


            pthread_mutex_unlock(
                &output_mutex
            );
        }


        if (fgets(
                payload,
                sizeof(payload),
                stdin
            ) == NULL)
        {
            /*
             * End-of-file on stdin.
             */
            if (atomic_load(
                    &client_running
                ))
            {
                send_message(
                    MSG_DISCONNECT,
                    ""
                );
            }


            break;
        }


        payload[
            strcspn(
                payload,
                "\n"
            )
        ] = '\0';


        if (!atomic_load(
                &client_running
            ))
        {
            break;
        }


        if (payload[0] == '\0')
        {
            continue;
        }


        /*
         * =================================================
         * /users
         * =================================================
         */
        if (strcmp(
                payload,
                "/users"
            ) == 0)
        {
            if (send_message(
                    MSG_LIST_USERS,
                    ""
                ) != 0)
            {
                fprintf(
                    stderr,
                    "Failed to request online users.\n"
                );


                atomic_store(
                    &client_running,
                    0
                );


                shutdown(
                    client_socket,
                    SHUT_RDWR
                );


                break;
            }


            continue;
        }


        /*
         * =================================================
         * /upload <local-path>
         * =================================================
         */
        if (strncmp(
                payload,
                "/upload",
                7
            ) == 0)
        {
            if (strncmp(
                    payload,
                    "/upload ",
                    8
                ) != 0 ||
                payload[8] == '\0')
            {
                safe_printf(
                    NULL,
                    "Usage: /upload <local-path>\n"
                );


                continue;
            }


            char upload_result[
                MESSAGE_MAX_SIZE + 1
            ];


            int upload_status =
                upload_client_upload(
                    client_socket,
                    payload + 8,
                    upload_result,
                    sizeof(upload_result)
                );


            if (pthread_mutex_lock(
                    &output_mutex
                ) == 0)
            {
                if (upload_status == 0)
                {
                    printf(
                        "\n[Upload] %s\n",
                        upload_result
                    );
                }
                else if (upload_status == 1)
                {
                    printf(
                        "\n[Upload Error] %s\n",
                        upload_result
                    );
                }
                else
                {
                    printf(
                        "\n[Upload Error] "
                        "Connection lost during upload.\n"
                    );
                }


                fflush(stdout);


                pthread_mutex_unlock(
                    &output_mutex
                );
            }


            /*
             * Negative upload result means the TCP
             * connection can no longer be trusted.
             */
            if (upload_status < 0)
            {
                atomic_store(
                    &client_running,
                    0
                );


                shutdown(
                    client_socket,
                    SHUT_RDWR
                );


                break;
            }


            continue;
        }


        /*
         * =================================================
         * /download <filename>
         * =================================================
         */
        if (strncmp(
                payload,
                "/download",
                9
            ) == 0)
        {
            /*
             * Only a shared filename is accepted.
             *
             * Example:
             *
             * /download test-upload.bin
             */
            if (strncmp(
                    payload,
                    "/download ",
                    10
                ) != 0 ||
                payload[10] == '\0')
            {
                safe_printf(
                    NULL,
                    "Usage: /download <filename>\n"
                );


                continue;
            }


            const char *filename =
                payload + 10;


            /*
             * Reject paths/path traversal locally before
             * sending anything to the server.
             */
            if (!is_valid_shared_filename(
                    filename
                ))
            {
                safe_printf(
                    NULL,
                    "Invalid shared filename.\n"
                );


                continue;
            }


            if (send_message(
                    MSG_DOWNLOAD,
                    filename
                ) != 0)
            {
                fprintf(
                    stderr,
                    "Failed to send download request.\n"
                );


                atomic_store(
                    &client_running,
                    0
                );


                shutdown(
                    client_socket,
                    SHUT_RDWR
                );


                break;
            }


            /*
             * Actual DOWNLOAD_READY + raw file bytes are
             * processed asynchronously by receiver_thread.
             */
            continue;
        }


        /*
         * =================================================
         * /msg <username> <message>
         * =================================================
         */
        if (strncmp(
                payload,
                "/msg",
                4
            ) == 0)
        {
            if (strncmp(
                    payload,
                    "/msg ",
                    5
                ) != 0)
            {
                safe_printf(
                    NULL,
                    "Usage: /msg <user> <message>\n"
                );


                continue;
            }


            char *request =
                payload + 5;


            char *separator =
                strchr(
                    request,
                    ' '
                );


            if (separator == NULL ||
                separator == request ||
                separator[1] == '\0')
            {
                safe_printf(
                    NULL,
                    "Usage: /msg <user> <message>\n"
                );


                continue;
            }


            size_t target_length =
                (size_t)(
                    separator -
                    request
                );


            if (target_length >=
                USERNAME_MAX_SIZE)
            {
                safe_printf(
                    NULL,
                    "Invalid target username.\n"
                );


                continue;
            }


            char target[
                USERNAME_MAX_SIZE
            ];


            memcpy(
                target,
                request,
                target_length
            );


            target[
                target_length
            ] = '\0';


            if (!is_valid_username(
                    target
                ))
            {
                safe_printf(
                    NULL,
                    "Invalid target username.\n"
                );


                continue;
            }


            if (strcmp(
                    target,
                    username
                ) == 0)
            {
                safe_printf(
                    NULL,
                    "You cannot privately "
                    "message yourself.\n"
                );


                continue;
            }


            if (send_message(
                    MSG_PRIVATE,
                    request
                ) != 0)
            {
                fprintf(
                    stderr,
                    "Failed to send private message.\n"
                );


                atomic_store(
                    &client_running,
                    0
                );


                shutdown(
                    client_socket,
                    SHUT_RDWR
                );


                break;
            }


            continue;
        }


        /*
         * =================================================
         * /quit
         * =================================================
         */
        if (strcmp(
                payload,
                "/quit"
            ) == 0)
        {
            if (send_message(
                    MSG_DISCONNECT,
                    ""
                ) != 0)
            {
                fprintf(
                    stderr,
                    "Failed to send disconnect request.\n"
                );


                atomic_store(
                    &client_running,
                    0
                );


                shutdown(
                    client_socket,
                    SHUT_RDWR
                );
            }


            /*
             * Do not close immediately.
             *
             * Receiver thread remains active until it
             * receives GOODBYE from the server.
             */
            break;
        }


        /*
         * =================================================
         * NORMAL PUBLIC CHAT
         * =================================================
         */
        if (send_message(
                MSG_CHAT,
                payload
            ) != 0)
        {
            fprintf(
                stderr,
                "Failed to send chat message.\n"
            );


            atomic_store(
                &client_running,
                0
            );


            shutdown(
                client_socket,
                SHUT_RDWR
            );


            break;
        }
    }


    /*
     * Wait for asynchronous receiver completion.
     */
    if (receiver_created)
    {
        pthread_join(
            receiver,
            NULL
        );
    }


    atomic_store(
        &client_running,
        0
    );


    cleanup_socket();


    pthread_mutex_destroy(
        &output_mutex
    );


    printf(
        "Disconnected from server.\n"
    );


    return EXIT_SUCCESS;
}
