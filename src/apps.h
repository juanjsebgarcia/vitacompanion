#pragma once

#include <stddef.h>

/* Writes a table of running applications into buffer. Returns the number of
 * characters written, or a negative error from the application manager. */
int apps_format_list(char *buffer, size_t capacity);
