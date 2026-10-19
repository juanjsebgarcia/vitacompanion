#include "caller.h"

#include <psp2kern/kernel/acmgr.h>
#include <psp2kern/kernel/sysroot.h>
#include <string.h>

/* Re-validated when SceShell restarts under a new PID. */
static SceUID allowed_pid = -1;

int vitacompanion_caller_allowed(SceUID pid)
{
#ifdef VITACOMPANION_KERNEL_ALLOW_ANY_CALLER
    (void)pid;
    return 1;
#else
    char title_id[32];

    if (pid >= 0 && pid == allowed_pid)
        return 1;

    memset(title_id, 0, sizeof(title_id));
    if (ksceKernelSysrootGetProcessTitleId(
            pid, title_id, sizeof(title_id) - 1) < 0 ||
        strcmp(title_id, VITACOMPANION_SHELL_TITLE_ID) != 0 ||
        !ksceSblACMgrIsSystemProgram(pid))
        return 0;

    allowed_pid = pid;
    return 1;
#endif
}
