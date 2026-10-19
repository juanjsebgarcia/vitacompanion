#pragma once

#include <vitacompanion_kernel.h>

#include <stdint.h>

#define SCREENSHOT_DIRECTORY "ux0:data/vitacompanion"
#define SCREENSHOT_PATH SCREENSHOT_DIRECTORY "/screenshot.png"

typedef enum {
    VITACOMPANION_SCREENSHOT_ERROR_NO_MEMORY = -0x7201,
    VITACOMPANION_SCREENSHOT_ERROR_INCOMPLETE_IO = -0x7202,
    VITACOMPANION_SCREENSHOT_ERROR_INVALID_FRAME = -0x7203,
    VITACOMPANION_SCREENSHOT_ERROR_COMPRESSION = -0x7204,
    VITACOMPANION_SCREENSHOT_ERROR_TIMEOUT = -0x7205
} vitacompanion_screenshot_error;

/* Microseconds spent in each stage of the last screenshot_save call. */
typedef struct {
    uint32_t capture_us;
    uint32_t read_us;
    uint32_t encode_us;
    uint32_t write_us;
    uint32_t total_us;
} screenshot_timings;

/*
 * Saves the displayed frame to SCREENSHOT_PATH as an RGB PNG. `buffer` is a
 * VITACOMPANION_SCREENSHOT_BUFFER_* request; `info` receives the capture's
 * size, format and what each display buffer held.
 */
int screenshot_save(uint32_t buffer, vitacompanion_screenshot_info *info,
    screenshot_timings *timings);
