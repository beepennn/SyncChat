#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "common/file_transfer.h"


int is_valid_shared_filename(
    const char *filename
)
{
    if (filename == NULL)
    {
        return 0;
    }

    size_t length =
        strlen(filename);

    if (length == 0 ||
        length >= FILE_NAME_MAX_SIZE)
    {
        return 0;
    }

    /*
     * Do not allow hidden files, "." or "..".
     */
    if (filename[0] == '.')
    {
        return 0;
    }

    /*
     * First character must be an alphanumeric
     * character or underscore.
     */
    unsigned char first =
        (unsigned char)filename[0];

    if (!isalnum(first) &&
        first != '_')
    {
        return 0;
    }

    for (size_t i = 0;
         i < length;
         i++)
    {
        unsigned char character =
            (unsigned char)filename[i];

        if (!isalnum(character) &&
            character != '_' &&
            character != '-' &&
            character != '.')
        {
            return 0;
        }
    }

    return 1;
}
