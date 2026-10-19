#pragma once

#include <stdint.h>

#define SCREENSHOT_DIRECTORY "ux0:data/vitacompanion"
#define SCREENSHOT_PATH SCREENSHOT_DIRECTORY "/screenshot.bmp"

typedef enum {
    VITACOMPANION_SCREENSHOT_ERROR_NO_MEMORY = -0x7201,
    VITACOMPANION_SCREENSHOT_ERROR_INCOMPLETE_IO = -0x7202,
    VITACOMPANION_SCREENSHOT_ERROR_INVALID_FRAME = -0x7203
} vitacompanion_screenshot_error;

/* Saves the displayed frame to SCREENSHOT_PATH as a 32-bit BMP. */
int screenshot_save(uint32_t *width, uint32_t *height);
