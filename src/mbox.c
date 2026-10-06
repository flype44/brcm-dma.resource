/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <exec/types.h>
#include <exec/libraries.h>

#include <proto/mailbox.h>

#include "mbox.h"

/*
    MB_RawCommand() of proto/mailbox.h (the SFD of this build has this one function: LVO -6, the buffer in a0). The buffer is the one of the
    property interface, in big endian (the resource swaps it in and out), with at most 480 words. When the call failed the first word comes
    back as 0xffffffff; the second one is the answer code of the firmware.
*/
static BOOL RawCommand(struct Library *MailboxBase, ULONG *command)
{
    MB_RawCommand(command);

    return command[0] != 0xffffffffUL && command[1] == MB_SUCCESS;
}

/* A request with one tag, one word in and one word out; the answer is in *value */
static BOOL OneValue(struct Library *MailboxBase, ULONG tag, ULONG *value)
{
    ULONG buffer[7];

    buffer[0] = sizeof(buffer);     /* length */
    buffer[1] = 0;                  /* request */
    buffer[2] = tag;
    buffer[3] = 4;                  /* size of the value buffer */
    buffer[4] = 0;                  /* request / response code of the tag */
    buffer[5] = *value;
    buffer[6] = 0;                  /* end tag */

    if (!RawCommand(MailboxBase, buffer))
        return FALSE;

    *value = buffer[5];

    return TRUE;
}

ULONG GetDMAChannels(struct Library *MailboxBase)
{
    ULONG mask = 0;

    if (!OneValue(MailboxBase, MB_GET_DMA_CHANNELS, &mask))
        return 0;

    return mask;
}

ULONG AllocateMemory(struct Library *MailboxBase, ULONG size, ULONG alignment, ULONG flags)
{
    ULONG buffer[9];

    buffer[0] = sizeof(buffer);
    buffer[1] = 0;
    buffer[2] = MB_ALLOCATE_MEMORY;
    buffer[3] = 12;
    buffer[4] = 0;
    buffer[5] = size;
    buffer[6] = alignment;
    buffer[7] = flags;
    buffer[8] = 0;

    if (!RawCommand(MailboxBase, buffer))
        return 0xffffffffUL;

    return buffer[5];
}

ULONG LockMemory(struct Library *MailboxBase, ULONG handle)
{
    if (!OneValue(MailboxBase, MB_LOCK_MEMORY, &handle))
        return 0;

    return handle;
}

ULONG UnlockMemory(struct Library *MailboxBase, ULONG handle)
{
    if (!OneValue(MailboxBase, MB_UNLOCK_MEMORY, &handle))
        return 0xffffffffUL;

    return handle;
}

ULONG ReleaseMemory(struct Library *MailboxBase, ULONG handle)
{
    if (!OneValue(MailboxBase, MB_RELEASE_MEMORY, &handle))
        return 0xffffffffUL;

    return handle;
}
