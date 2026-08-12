#include "common/protocol.h"

int is_valid_message_type(uint32_t type)
{
    switch (type)
    {
        case MSG_CHAT:
        case MSG_DISCONNECT:
        case MSG_UPLOAD:
        case MSG_DOWNLOAD:
        case MSG_LIST_FILES:
        case MSG_RESPONSE:
        case MSG_ERROR:
            return 1;

        default:
            return 0;
    }
}
