#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/network_io.h"
#include "common/protocol.h"
#include "server/server_config.h"


static int client_socket = -1;

static atomic_int client_running = 1;

static pthread_mutex_t output_mutex =
    PTHREAD_MUTEX_INITIALIZER;


/*
 * Print text while preventing multiple threads
 * from writing to stdout simultaneously.
 */
static void safe_printf(const char *prefix, const char *message)
{
    pthread_mutex_lock(&output_mutex);

    if (prefix != NULL)
    {
        printf("%s", prefix);
    }

    if (message != NULL)
    {
        printf("%s", message);
    }

    fflush(stdout);

    pthread_mutex_unlock(&output_mutex);
}


/*
 * Send one framed application message.
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

    payload_length = strlen(payload);

    if (payload_length > MESSAGE_MAX_SIZE)
    {
        return -1;
    }

    header.type = htonl(type);
    header.length = htonl(
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

    return send_all(
        client_socket,
        payload,
        payload_length
    );
}


/*
 * Receive one framed application message.
 *
 * Return values:
 *
 *   0  message received
 *   1  peer closed connection
 *  -1  protocol/socket error
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

    int result = recv_all(
        client_socket,
        &header,
        sizeof(header)
    );

    if (result != 0)
    {
        return result;
    }

    type = ntohl(header.type);
    length = ntohl(header.length);

    if (!is_valid_message_type(type))
    {
        fprintf(
            stderr,
            "Invalid message type received: %u\n",
            type
        );

        return -1;
    }

    if (length > MESSAGE_MAX_SIZE)
    {
        fprintf(
            stderr,
            "Received message exceeds maximum size.\n"
        );

        return -1;
    }

    if ((size_t)length >= payload_size)
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

    server_address.sin_family = AF_INET;
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


static void cleanup_socket(void)
{
    if (client_socket >= 0)
    {
        close(client_socket);
        client_socket = -1;
    }
}


/*
 * Dedicated receiver thread.
 *
 * After login succeeds, this thread becomes the
 * only thread responsible for receiving messages
 * from the server.
 */
static void *receiver_thread(void *argument)
{
    (void)argument;

    char payload[MESSAGE_MAX_SIZE + 1];
    uint32_t message_type;

    while (atomic_load(&client_running))
    {
        int result = receive_message(
            &message_type,
            payload,
            sizeof(payload)
        );

        if (result == 1)
        {
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

        if (result < 0)
        {
            /*
             * shutdown() during normal termination may
             * also cause recv() to return an error.
             */
            if (atomic_load(&client_running))
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

        pthread_mutex_lock(
            &output_mutex
        );

        printf("\n");

        switch (message_type)
        {
            case MSG_BROADCAST:
                printf(
                    "%s\n",
                    payload
                );
                break;

            case MSG_RESPONSE:
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

            case MSG_ERROR:
                printf(
                    "[Error] %s\n",
                    payload
                );
                break;

            case MSG_USERLIST:
                printf(
                    "[Online Users]\n%s\n",
                    payload
                );
                break;

            default:
                printf(
                    "[Server message type %u] %s\n",
                    message_type,
                    payload
                );
                break;
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
    char payload[MESSAGE_MAX_SIZE + 1];
    char username[USERNAME_MAX_SIZE];

    uint32_t message_type;

    pthread_t receiver;

    int receiver_created = 0;


    if (connect_to_server() != 0)
    {
        return EXIT_FAILURE;
    }

    printf(
        "Connected to SyncChat server.\n"
    );


    /*
     * Login phase remains synchronous.
     *
     * The receiver thread is intentionally not
     * started until authentication succeeds.
     */
    int result = receive_message(
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

    if (message_type != MSG_RESPONSE ||
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


    while (1)
    {
        printf("Username: ");
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
            strcspn(username, "\n")
        ] = '\0';

        if (!is_valid_username(username))
        {
            printf(
                "Invalid username. "
                "Use letters, digits, and underscore only.\n"
            );

            continue;
        }

        break;
    }


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


    result = receive_message(
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


    if (message_type == MSG_ERROR)
    {
        printf(
            "Login rejected: %s\n",
            payload
        );

        cleanup_socket();

        return EXIT_FAILURE;
    }


    if (message_type != MSG_RESPONSE ||
        strncmp(
            payload,
            "LOGIN_SUCCESS",
            strlen("LOGIN_SUCCESS")
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
        "Commands: /users /quit\n"
    );


    /*
     * From this point onward:
     *
     * main thread     -> send
     * receiver thread -> receive
     */
    atomic_store(
        &client_running,
        1
    );

    int pthread_result = pthread_create(
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


    while (atomic_load(
            &client_running
        ))
    {
        pthread_mutex_lock(
            &output_mutex
        );

        printf(
            "%s> ",
            username
        );

        fflush(stdout);

        pthread_mutex_unlock(
            &output_mutex
        );


        if (fgets(
                payload,
                sizeof(payload),
                stdin
            ) == NULL)
        {
            /*
             * stdin EOF: request a graceful disconnect.
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
            strcspn(payload, "\n")
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
         * Request current online-user list.
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
             * Do not close the socket yet.
             *
             * The receiver thread remains alive so that
             * it can receive the server's GOODBYE.
             */
            break;
        }


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
     * Wait for the receiver to finish.
     *
     * On /quit, the server should send GOODBYE and
     * subsequently close its side of the connection.
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
