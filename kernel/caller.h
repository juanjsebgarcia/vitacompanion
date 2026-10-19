#pragma once

#include <psp2kern/types.h>

/* SceShell's title ID, as taiHEN's `*main` section names it. */
#define VITACOMPANION_SHELL_TITLE_ID "main"

/* Call with the state mutex held. */
int vitacompanion_caller_allowed(SceUID pid);
