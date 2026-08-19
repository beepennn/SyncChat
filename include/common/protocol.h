#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MESSAGE_MAX_SIZE 4096
#define USERNAME_MAX_SIZE 32

/*
 * Client -> Server messages
 */
#define MSG_LOGIN        1
#define MSG_CHAT         2
#define MSG_DISCONNECT   3
#define MSG_UPLOAD       4
#define MSG_DOWNLOAD     5
#define MSG_LIST_FILES   6

/*
 * Server -> Client messages
 */
#define MSG_RESPONSE     7
#define MSG_ERROR        8
#define MSG_BROADCAST    9
#define MSG_USERLIST     10

/*
 * Additional chat operations.
 */
#define MSG_LIST_USERS   11

/*
 * Used for both:
 *
 * Client -> Server:
 *     "target_username message"
 *
 * Server -> Client:
 *     "sender_username: message"
 */
#define MSG_PRIVATE      12


typedef struct
{
    uint32_t type;
    uint32_t length;

} message_header_t;


int is_valid_message_type(uint32_t type);

int is_valid_username(const char *username);

#endif
