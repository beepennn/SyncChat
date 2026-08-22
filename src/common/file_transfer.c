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
     * Shared files cannot be hidden and cannot begin
     * with punctuation.
     */
    unsigned char first =
        (unsigned char)filename[0];


    if (!isalnum(first) &&
        filename[0] != '_')
    {
        return 0;
    }


    /*
     * Reject traversal-like dot-dot sequences even
     * though path separators are independently banned.
     *
     * This intentionally uses a stricter policy than
     * Linux itself because SyncChat only needs simple
     * shared basenames.
     */
    if (strstr(
            filename,
            ".."
        ) != NULL)
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
