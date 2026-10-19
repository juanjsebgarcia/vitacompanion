#pragma once

#include <stdint.h>

#define VITACOMPANION_KERNEL_ABI_VERSION 4
#define VITACOMPANION_KERNEL_MODULE_NAME "vitacompanion_kernel"
/* Returned when a process other than SceShell calls a guarded export. */
#define VITACOMPANION_KERNEL_ERROR_NOT_PERMITTED ((int)0x80AC0001)
/* This syscall lost its fifth, stack-passed argument on device. */
#define VITACOMPANION_TOUCH_ACTIVE_FLAG 0x100

int vitaCompanionKernelGetApiVersion(void);
int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed);
int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active);
int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y);
int vitaCompanionKernelReset(void);
int vitaCompanionKernelRequestReboot(void);
int vitaCompanionKernelForceReboot(void);
