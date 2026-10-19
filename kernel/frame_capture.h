#pragma once

#include <vitacompanion_kernel.h>

int vitacompanion_screenshot_init(void);
void vitacompanion_screenshot_term(void);
int vitacompanion_screenshot_capture(vitacompanion_screenshot_info *info);
int vitacompanion_screenshot_read(uint32_t offset, void *buffer,
    uint32_t size);
int vitacompanion_screenshot_release(void);
