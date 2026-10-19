#pragma once

#include <stdint.h>

#define VITACOMPANION_KERNEL_ABI_VERSION 7
#define VITACOMPANION_KERNEL_MODULE_NAME "vitacompanion_kernel"
/* Returned when a process other than SceShell calls a guarded export. */
#define VITACOMPANION_KERNEL_ERROR_NOT_PERMITTED ((int)0x80AC0001)
#define VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT ((int)0x80AC0002)
/* No frame is on screen, or its size or layout is unusable. */
#define VITACOMPANION_KERNEL_ERROR_NO_FRAME ((int)0x80AC0003)
/* This syscall lost its fifth, stack-passed argument on device. */
#define VITACOMPANION_TOUCH_ACTIVE_FLAG 0x100

/* Which of the display's two frame buffers to capture. */
#define VITACOMPANION_SCREENSHOT_BUFFER_AUTO 0  /* 0, or 1 if 0 is empty */
#define VITACOMPANION_SCREENSHOT_BUFFER_0 1
#define VITACOMPANION_SCREENSHOT_BUFFER_1 2

/* What the display reported for one of its frame buffers. */
typedef struct vitacompanion_screenshot_buffer {
    int32_t result;
    int32_t pid;
    uint32_t paddr;
    uint32_t pixelformat;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    char title_id[12];      /* Owning process, or empty if unknown */
} vitacompanion_screenshot_buffer;

/*
 * A capture is 32-bit B,G,R,X, top row first, `pitch` pixels per row.
 * `buffer` is read as a VITACOMPANION_SCREENSHOT_BUFFER_* request and
 * returned as the index captured (0 or 1).
 */
typedef struct vitacompanion_screenshot_info {
    uint32_t buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t source_pixelformat;
    uint32_t swapped_red_blue;  /* 1 if the capture holds R,G,B,X instead */
    vitacompanion_screenshot_buffer buffers[2];
} vitacompanion_screenshot_info;

int vitaCompanionKernelGetApiVersion(void);
int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed);
int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active);
int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y);
int vitaCompanionKernelReset(void);
int vitaCompanionKernelRequestReboot(void);
int vitaCompanionKernelForceReboot(void);
int vitaCompanionKernelScreenshotCapture(
    vitacompanion_screenshot_info *info);
int vitaCompanionKernelScreenshotRead(uint32_t offset, void *buffer,
    uint32_t size);
int vitaCompanionKernelScreenshotRelease(void);
