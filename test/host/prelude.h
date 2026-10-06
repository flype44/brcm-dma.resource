/* A few AmigaOS types for the host: only what src/tags.c and the tests need */
#ifndef HOST_PRELUDE_H
#define HOST_PRELUDE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint16_t UWORD;
typedef int16_t WORD;
typedef uint8_t UBYTE;
typedef int BOOL;
typedef void VOID;
typedef void *APTR;
typedef const char *CONST_STRPTR;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
typedef ULONG Tag;
struct TagItem { Tag ti_Tag; ULONG ti_Data; };
#define TAG_DONE   (0UL)
#define TAG_END    (0UL)
#define TAG_IGNORE (1UL)
#define TAG_MORE   (2UL)
#define TAG_SKIP   (3UL)
#define TAG_USER   ((ULONG)(1UL << 31))
struct Task;
struct Library { int dummy; };
struct SignalSemaphore { int dummy; };
struct ExecBase;
#define REGARG(arg, reg) arg
#endif
