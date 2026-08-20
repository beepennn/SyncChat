#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "logger/logger_client.h"
#include "server/client_handler.h"
#include "server/client_manager.h"
#include "server/server_config.h"


static volatile sig_atomic_t running = 1;

static int server_socket = -1;


/*
 * Handle SIGINT and SIGTERM.
 *
 * Do not perform logging inside a signal handler
 * because socket(), snprintf(), etc. are not
 * async-signal-safe.
 */
static void handle_signal(
    int signal_number
)
{
    (void)signal_number;

    running = 0;
}


/*
 * Release server resources.
 */
static void cleanup(void)
{
    if (server_socket >= 0)
    {
        if (close(
                server_socket
            ) != 0)
        {
            perror(
                "close server socket"
            );

            logger_client_log(
                LOG_ERROR,
                "SERVER",
                "Failed to close listening socket"
            );
        }

        server_socket = -1;
    }
}


/*
 * Create, configure, bind and listen on the
 * server TCP socket.
 */
static int create_server_socket(void)
{
    int reuse_address = 1;

    struct sockaddr_in server_address;


    server_socket = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (server_socket < 0)
    {
        perror("socket");

        logger_client_log(
            LOG_ERROR,
            "SERVER",
            "socket() failed: %s",
            strerror(errno)
        );

        return -1;
    }


    if (setsockopt(
            server_socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)
        ) < 0)
    {
        perror("setsockopt");

        logger_client_log(
            LOG_ERROR,
            "SERVER",
            "setsockopt(SO_REUSEADDR) failed: %s",
            strerror(errno)
        );

        close(server_socket);

        server_socket = -1;

        return -1;
    }


    memset(
        &server_address,
        0,
        sizeof(server_address)
    );


    server_address.sin_family =
        AF_INET;

    server_address.sin_addr.s_addr =
        htonl(INADDR_ANY);

    server_address.sin_port =
        htons(SERVER_PORT);


    if (bind(
            server_socket,
            (struct sockaddr *)&server_address,
            sizeof(server_address)
        ) < 0)
    {
        perror("bind");

        logger_client_log(
            LOG_ERROR,
            "SERVER",
            "bind() failed on port %d: %s",
            SERVER_PORT,
            strerror(errno)
        );

        close(server_socket);

        server_socket = -1;

        return -1;
    }


    if (listen(
            server_socket,
            SERVER_BACKLOG
        ) < 0)
    {
        perror("listen");

        logger_client_log(
            LOG_ERROR,
            "SERVER",
            "listen() failed: %s",
            strerror(errno)
        );

        close(server_socket);

        server_socket = -1;

        return -1;
    }


    return 0;
}


int main(void)
{
    struct sockaddr_in client_address;

    socklen_t client_address_length;

    struct sigaction action;


    /*
     * Configure signal handling.
     */
    memset(
        &action,
        0,
        sizeof(action)
    );


    action.sa_handler =
        handle_signal;


    if (sigemptyset(
            &action.sa_mask
        ) != 0)
    {
        perror("sigemptyset");

        logger_client_log(
            LOG_ERROR,
            "SERVER",
            "sigemptyset() failed"
        );

        return EXIT_FAILURE;
    }


    /*
     * No SA_RESTART because SIGINT/SIGTERM
     * should interrupt accept().
     */
    action.sa_flags = 0;


    if (sigaction(
            SIGINT,
            &action,
            NULL
        ) != 0)
    {
        perror(
            "sigaction SIGINT"
        );

        logger_client_log(
            LOG_ERROR,
            "SERVER",
            "Failed to configure SIGINT handler"
        );

        return EXIT_FAILURE;
    }


    if (sigaction(
            SIGTERM,
            &action,
            NULL
        ) != 0)
    {
        perror(
            "sigaction SIGTERM"
        );

        logger_client_log(
            LOG_ERROR,
            "SERVER",
            "Failed to configure SIGTERM handler"
        );

        return EXIT_FAILURE;
    }


    /*
     * Initialize the shared client registry before
     * accepting client connections.
     */
    if (client_manager_init() != 0)
    {
        fprintf(
            stderr,
            "Failed to initialize client manager.\n"
        );

        logger_client_log(
            LOG_ERROR,
            "SERVER",
            "Client manager initialization failed"
        );

        return EXIT_FAILURE;
    }


    if (create_server_socket() != 0)
    {
        return EXIT_FAILURE;
    }


    printf(
        "Concurrent TCP server started.\n"
        "Listening on port %d.\n",
        SERVER_PORT
    );


    logger_client_log(
        LOG_INFO,
        "SERVER",
        "Server started on TCP port %d",
        SERVER_PORT
    );


    while (running)
    {
        int client_socket;


        memset(
            &client_address,
            0,
            sizeof(client_address)
        );


        client_address_length =
            sizeof(client_address);


        /*
         * Main thread only accepts connections.
         */
        client_socket = accept(
            server_socket,
            (struct sockaddr *)&client_address,
            &client_address_length
        );


        if (client_socket < 0)
        {
            if (errno == EINTR)
            {
                /*
                 * A shutdown signal may have
                 * interrupted accept().
                 */
                if (!running)
                {
                    break;
                }

                continue;
            }


            perror("accept");

            logger_client_log(
                LOG_ERROR,
                "SERVER",
                "accept() failed: %s",
                strerror(errno)
            );

            break;
        }


        char client_ip[
            INET_ADDRSTRLEN
        ];


        if (inet_ntop(
                AF_INET,
                &client_address.sin_addr,
                client_ip,
                sizeof(client_ip)
            ) == NULL)
        {
            strncpy(
                client_ip,
                "unknown",
                sizeof(client_ip) - 1
            );

            client_ip[
                sizeof(client_ip) - 1
            ] = '\0';
        }


        unsigned int client_port =
            ntohs(
                client_address.sin_port
            );


        printf(
            "Accepted client %s:%u\n",
            client_ip,
            client_port
        );


        logger_client_log(
            LOG_INFO,
            "SERVER",
            "Accepted client connection from %s:%u",
            client_ip,
            client_port
        );


        /*
         * Allocate thread context dynamically.
         */
        client_context_t *context =
            malloc(
                sizeof(client_context_t)
            );


        if (context == NULL)
        {
            fprintf(
                stderr,
                "Failed to allocate client context.\n"
            );


            logger_client_log(
                LOG_ERROR,
                "SERVER",
                "Failed to allocate client context for %s:%u",
                client_ip,
                client_port
            );


            close(client_socket);

            continue;
        }


        context->socket_fd =
            client_socket;


        pthread_t thread;


        int thread_result =
            pthread_create(
                &thread,
                NULL,
                client_handler,
                context
            );


        if (thread_result != 0)
        {
            fprintf(
                stderr,
                "pthread_create failed: %s\n",
                strerror(thread_result)
            );


            logger_client_log(
                LOG_ERROR,
                "SERVER",
                "pthread_create failed for %s:%u: %s",
                client_ip,
                client_port,
                strerror(thread_result)
            );


            free(context);

            close(client_socket);

            continue;
        }


        /*
         * Client handler owns the context/socket
         * from this point onward.
         */
        thread_result =
            pthread_detach(thread);


        if (thread_result != 0)
        {
            fprintf(
                stderr,
                "pthread_detach failed: %s\n",
                strerror(thread_result)
            );


            logger_client_log(
                LOG_WARN,
                "SERVER",
                "pthread_detach failed for thread %lu: %s",
                (unsigned long)thread,
                strerror(thread_result)
            );
        }


        printf(
            "Created client thread [%lu].\n",
            (unsigned long)thread
        );


        logger_client_log(
            LOG_DEBUG,
            "SERVER",
            "Created client handler thread %lu",
            (unsigned long)thread
        );
    }


    logger_client_log(
        LOG_INFO,
        "SERVER",
        "Server shutdown initiated"
    );


    cleanup();


    printf(
        "Concurrent TCP server stopped.\n"
    );


    logger_client_log(
        LOG_INFO,
        "SERVER",
        "Server stopped"
    );


    return EXIT_SUCCESS;
}
