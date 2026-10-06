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

/*
    The few requests of the VideoCore mailbox that the engine needs, written on RawCommand() of mailbox.resource and nothing else:
    the one function that every version of the resource has had (LVO -6). The other functions of the resource are helpers for
    applications; a resource in the ROM stays on the low level and does not depend on a version newer than 1.0.
    `MailboxBase` is the base given by OpenResource(MAILBOXNAME).
*/

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
#define MEM_FLAG_DIRECT (1 << 2)            /* 0xC alias, uncached */
#endif

#ifndef MEM_FLAG_COHERENT
#define MEM_FLAG_COHERENT (2 << 2)          /* 0x8 alias, cache coherent */
#endif

#ifndef MEM_FLAG_HINT_PERMALOCK
#define MEM_FLAG_HINT_PERMALOCK (1 << 6)    /* likely to be locked for long periods of time */
#endif

/* The channels of the DMA that the firmware leaves to the ARM, as a bit mask; 0 when the request failed */
ULONG GetDMAChannels(struct Library *MailboxBase);

/* GPU memory (the flags are MEM_FLAG_* of resources/mailbox.h): the handle, or 0xffffffff when the request failed */
ULONG AllocateMemory(struct Library *MailboxBase, ULONG size, ULONG alignment, ULONG flags);

/* The bus address of the memory; 0 when the request failed */
ULONG LockMemory(struct Library *MailboxBase, ULONG handle);

/* Unlocks the memory (it keeps its contents, but may move); 0 when it went well. The sequence is lock, unlock, release. */
ULONG UnlockMemory(struct Library *MailboxBase, ULONG handle);

/* Gives the memory back; 0 when it went well */
ULONG ReleaseMemory(struct Library *MailboxBase, ULONG handle);

#endif /* _MBOX_H */
