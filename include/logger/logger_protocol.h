#ifndef LOGGER_PROTOCOL_H
#define LOGGER_PROTOCOL_H

#include <stdint.h>
#include <sys/types.h>

#define LOGGER_SOCKET_PATH "/tmp/syncchat_logger.sock"
#define LOGGER_LOG_FILE    "logs/server.log"

#define LOGGER_MAGIC       0x53434C47U
#define LOGGER_VERSION     1

#define LOGGER_MESSAGE_MAX 1024
#define LOGGER_SOURCE_MAX  32

typedef enum
{
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t level;

    pid_t sender_pid;

    char source[LOGGER_SOURCE_MAX];
    char message[LOGGER_MESSAGE_MAX];

} log_packet_t;

#endif
