#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MESSAGE_MAX_SIZE 4096

#define MSG_CHAT        1
#define MSG_DISCONNECT  2
#define MSG_UPLOAD      3
#define MSG_DOWNLOAD    4
#define MSG_LIST_FILES  5
#define MSG_RESPONSE    6
#define MSG_ERROR       7

typedef struct
{
    uint32_t type;
    uint32_t length;
} message_header_t;

int is_valid_message_type(uint32_t type);

#endif
