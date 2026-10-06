#include <exec/types.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>
#include <proto/exec.h>

int main(int, struct WBStartup *);

/* Startup code including workbench message support, same pattern used by
   EmuControl's own src/startup.c -- kept consistent across this ecosystem. */
int __attribute__((used)) _start()
{
    struct ExecBase *SysBase = *(struct ExecBase **)4;
    struct Process *p = NULL;
    struct WBStartup *wbmsg = NULL;
    int ret = 0;

    p = (struct Process *)SysBase->ThisTask;

    if (p->pr_CLI == 0)
    {
        WaitPort(&p->pr_MsgPort);
        wbmsg = (struct WBStartup *)GetMsg(&p->pr_MsgPort);
    }

    ret = main(wbmsg ? 1 : 0, wbmsg);

    if (wbmsg)
    {
        Forbid();
        ReplyMsg((struct Message *)wbmsg);
    }

    return ret;
}
