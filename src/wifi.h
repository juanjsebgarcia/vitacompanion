#pragma once

#include <stddef.h>

#define WIFI_OFF_MAX_SECONDS 300
#define WIFI_FORCE_MAX_SECONDS 3600

void wifi_tick(void);
void wifi_end(void);
int wifi_off_for(unsigned int seconds);
void wifi_force_set(unsigned int interval_seconds);
void wifi_status(char *buf, size_t size);
