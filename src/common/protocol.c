#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "common/protocol.h"


int is_valid_message_type(uint32_t type)
{
    switch (type)
    {
        case MSG_LOGIN:
        case MSG_CHAT:
        case MSG_DISCONNECT:
        case MSG_UPLOAD:
        case MSG_DOWNLOAD:
        case MSG_LIST_FILES:
        case MSG_RESPONSE:
        case MSG_ERROR:
        case MSG_BROADCAST:
        case MSG_USERLIST:
        case MSG_LIST_USERS:
            return 1;

        default:
            return 0;
    }
}


int is_valid_username(const char *username)
{
    if (username == NULL)
    {
        return 0;
    }

    size_t length = strlen(username);

    /*
     * One byte is reserved for '\0'.
     */
    if (length == 0 ||
        length >= USERNAME_MAX_SIZE)
    {
        return 0;
    }

    for (size_t i = 0;
         i < length;
         i++)
    {
        unsigned char character =
            (unsigned char)username[i];

        if (!isalnum(character) &&
            character != '_')
        {
            return 0;
        }
    }

    return 1;
}
