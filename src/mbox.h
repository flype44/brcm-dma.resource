/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _MBOX_H
#define _MBOX_H

#include <exec/types.h>
#include <exec/libraries.h>

#ifndef MAILBOXNAME
#define MAILBOXNAME "mailbox.resource"
#endif

#ifndef MB_SUCCESS
#define MB_SUCCESS (0x80000000)
#endif

#ifndef MB_GET_DMA_CHANNELS
#define MB_GET_DMA_CHANNELS (0x00060001)
#endif

#ifndef MB_ALLOCATE_MEMORY
#define MB_ALLOCATE_MEMORY (0x0003000c)
#endif

#ifndef MB_RELEASE_MEMORY
#define MB_RELEASE_MEMORY (0x0003000f)
#endif

#ifndef MB_LOCK_MEMORY
#define MB_LOCK_MEMORY (0x0003000d)
#endif

#ifndef MB_UNLOCK_MEMORY
#define MB_UNLOCK_MEMORY (0x0003000e)
#endif

#ifndef MEM_FLAG_DIRECT
#define MEM_FLAG_DIRECT (1 << 2)
#endif

#ifndef MEM_FLAG_COHERENT
#define MEM_FLAG_COHERENT (2 << 2)
#endif

#ifndef MEM_FLAG_HINT_PERMALOCK
#define MEM_FLAG_HINT_PERMALOCK (1 << 6)
#endif

ULONG GetDMAChannels(struct Library *MailboxBase);

ULONG AllocateMemory(struct Library *MailboxBase, ULONG size, ULONG alignment, ULONG flags);

ULONG ReleaseMemory(struct Library *MailboxBase, ULONG handle);

ULONG LockMemory(struct Library *MailboxBase, ULONG handle);

ULONG UnlockMemory(struct Library *MailboxBase, ULONG handle);

#endif /* _MBOX_H */
