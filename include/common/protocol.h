#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MESSAGE_MAX_SIZE 4096
#define USERNAME_MAX_SIZE 32

/*
 * Client -> Server
 */
#define MSG_LOGIN        1
#define MSG_CHAT         2
#define MSG_DISCONNECT   3
#define MSG_UPLOAD       4
#define MSG_DOWNLOAD     5
#define MSG_LIST_FILES   6

/*
 * Server -> Client
 */
#define MSG_RESPONSE     7
#define MSG_ERROR        8
#define MSG_BROADCAST    9
#define MSG_USERLIST     10

typedef struct
{
    uint32_t type;
    uint32_t length;
} message_header_t;


/*
 * Validate whether a message type is supported.
 */
int is_valid_message_type(uint32_t type);


/*
 * Validate a username according to the application rules.
 *
 * Rules:
 *   - 1 to USERNAME_MAX_SIZE - 1 characters
 *   - Letters: A-Z / a-z
 *   - Digits: 0-9
 *   - Underscore: _
 *   - No spaces
 *   - No special characters
 */
int is_valid_username(const char *username);

#endif
