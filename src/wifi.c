#include "wifi.h"

#include "log.h"

#include <psp2/net/netctl.h>
#include <psp2/registrymgr.h>
#include <stdarg.h>
#include <stdio.h>

/* SceWlan is exported to user mode but not declared in vita-headers. The
 * request numbers and configuration bits come from the reverse-engineered
 * driver (shoui520/psp2, wlan_robin.c) and are unverified on retail firmware.
 * Requests 2 and 3 (flight mode) are deliberately never sent. */
int sceWlanSetConfiguration(int request);
unsigned int sceWlanGetConfiguration(void);

#define WLAN_REQUEST_OFF 0
#define WLAN_REQUEST_ON 1

#define WLAN_CONFIG_ON 0x1
#define WLAN_CONFIG_FLIGHT 0x2
#define WLAN_CONFIG_POWER_SAVE 0x100
#define WLAN_CONFIG_POWER_EVENT_OFF 0x200
#define WLAN_CONFIG_STOPPED 0x400

/* Seconds between accepting `wifi off` and acting on it, so the command's
 * reply reaches the client before the link drops. */
#define WIFI_OFF_GRACE_SECONDS 2
#define WIFI_RESTORE_RETRY_SECONDS 5

typedef enum
{
    WIFI_IDLE,
    WIFI_OFF_SCHEDULED,
    WIFI_OFF,
    WIFI_RESTORING,
} wifi_state;

/* Written by the command thread, consumed by wifi_tick() on the net thread.
 * wifi_tick() is the only writer of everything else, so a timed `off` is
 * always undone by the thread that survives the network going down. */
static volatile unsigned int off_request_seconds;
static volatile unsigned int force_interval_seconds;

static volatile int tick_running;
static volatile wifi_state state = WIFI_IDLE;
static volatile unsigned int countdown;
static volatile unsigned int off_seconds;
static volatile int last_result;
static unsigned int force_countdown;

static int wlan_config_valid(unsigned int config)
{
    return (int)config >= 0;
}

static void wlan_set(int request)
{
    int result = sceWlanSetConfiguration(request);

    LOG("sceWlanSetConfiguration(%d): 0x%08X\n", request, result);
    last_result = result;
}

static void tick_timed_off(void)
{
    switch (state)
    {
    case WIFI_IDLE:
        if (off_request_seconds > 0)
        {
            off_seconds = off_request_seconds;
            off_request_seconds = 0;
            countdown = WIFI_OFF_GRACE_SECONDS;
            state = WIFI_OFF_SCHEDULED;
        }
        break;

    case WIFI_OFF_SCHEDULED:
        if (--countdown > 0)
            break;
        wlan_set(WLAN_REQUEST_OFF);
        countdown = off_seconds;
        state = WIFI_OFF;
        break;

    case WIFI_OFF:
        if (--countdown > 0)
            break;
        countdown = 0;
        state = WIFI_RESTORING;
        /* fall through */

    case WIFI_RESTORING:
    {
        unsigned int config = sceWlanGetConfiguration();

        if (wlan_config_valid(config) && (config & WLAN_CONFIG_ON))
        {
            state = WIFI_IDLE;
            break;
        }
        if (countdown == 0)
        {
            wlan_set(WLAN_REQUEST_ON);
            countdown = WIFI_RESTORE_RETRY_SECONDS;
        }
        else
        {
            countdown--;
        }
        break;
    }
    }
}

static void tick_force(void)
{
    unsigned int interval = force_interval_seconds;
    unsigned int config;

    if (interval == 0)
    {
        force_countdown = 0;
        return;
    }

    if (force_countdown == 0 || force_countdown > interval)
        force_countdown = interval;
    if (--force_countdown > 0)
        return;

    /* Leave alone a timed `off`, flight mode, and a shutdown by a power
     * event (suspend, or a game asking for GPU boost). */
    if (state != WIFI_IDLE)
        return;
    config = sceWlanGetConfiguration();
    if (!wlan_config_valid(config) ||
        (config & (WLAN_CONFIG_ON | WLAN_CONFIG_FLIGHT |
            WLAN_CONFIG_POWER_EVENT_OFF)))
        return;

    LOG("wifi force: Wi-Fi is off (config 0x%08X), turning it on\n", config);
    wlan_set(WLAN_REQUEST_ON);
}

void wifi_tick(void)
{
    tick_running = 1;
    tick_timed_off();
    tick_force();
}

void wifi_end(void)
{
    tick_running = 0;
    force_interval_seconds = 0;

    /* Never leave the console without Wi-Fi when the plugin stops. */
    if (state == WIFI_OFF || state == WIFI_RESTORING)
        wlan_set(WLAN_REQUEST_ON);
    state = WIFI_IDLE;
    off_request_seconds = 0;
}

int wifi_off_for(unsigned int seconds)
{
    if (!tick_running)
        return -1;
    if (state != WIFI_IDLE || off_request_seconds > 0)
        return -2;

    off_request_seconds = seconds;
    return 0;
}

void wifi_force_set(unsigned int interval_seconds)
{
    force_interval_seconds = interval_seconds;
}

static const char *netctl_state_name(int netctl_state)
{
    switch (netctl_state)
    {
    case SCE_NETCTL_STATE_DISCONNECTED:
        return "disconnected";
    case SCE_NETCTL_STATE_CONNECTING:
        return "connecting";
    case SCE_NETCTL_STATE_FINALIZING:
        return "finalizing";
    case SCE_NETCTL_STATE_CONNECTED:
        return "connected";
    default:
        return "unknown";
    }
}

static void appendf(char *buf, size_t size, size_t *used, const char *fmt,
    ...)
{
    va_list ap;
    int n;

    if (*used >= size)
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, size - *used, fmt, ap);
    va_end(ap);
    if (n > 0)
        *used += (size_t)n < size - *used ? (size_t)n : size - *used - 1;
}

static void append_registry_int(char *buf, size_t size, size_t *used,
    const char *category, const char *name)
{
    int value = 0;
    int result = sceRegMgrGetKeyInt(category, name, &value);

    if (result < 0)
        appendf(buf, size, used, " %s=unavailable(0x%08X)", name, result);
    else
        appendf(buf, size, used, " %s=%d", name, value);
}

void wifi_status(char *buf, size_t size)
{
    unsigned int config = sceWlanGetConfiguration();
    int netctl_state = -1;
    int netctl_result = sceNetCtlInetGetState(&netctl_state);
    size_t used = 0;

    if (wlan_config_valid(config))
        appendf(buf, size, &used, "Wi-Fi: %s (config 0x%08X:%s%s%s%s%s)\n",
            (config & WLAN_CONFIG_ON) ? "on" : "off", config,
            (config & WLAN_CONFIG_ON) ? " on" : "",
            (config & WLAN_CONFIG_FLIGHT) ? " flight-mode" : "",
            (config & WLAN_CONFIG_POWER_SAVE) ? " power-save" : "",
            (config & WLAN_CONFIG_POWER_EVENT_OFF) ? " off-by-power-event" : "",
            (config & WLAN_CONFIG_STOPPED) ? " stopped" : "");
    else
        appendf(buf, size, &used, "Wi-Fi: unavailable (0x%08X)\n", config);

    appendf(buf, size, &used, "Registry:");
    append_registry_int(buf, size, &used, "/CONFIG/NET", "wifi_flag");
    append_registry_int(buf, size, &used, "/CONFIG/NET", "wifi_power_save");
    append_registry_int(buf, size, &used, "/CONFIG/SYSTEM", "flight_mode");
    appendf(buf, size, &used, "\n");

    if (netctl_result < 0)
        appendf(buf, size, &used, "Network: unavailable (0x%08X)\n",
            netctl_result);
    else
        appendf(buf, size, &used, "Network: %s\n",
            netctl_state_name(netctl_state));

    switch (state)
    {
    case WIFI_IDLE:
        appendf(buf, size, &used, "Timed off: none\n");
        break;
    case WIFI_OFF_SCHEDULED:
        appendf(buf, size, &used, "Timed off: starting, for %us\n",
            off_seconds);
        break;
    case WIFI_OFF:
        appendf(buf, size, &used, "Timed off: back on in %us\n", countdown);
        break;
    case WIFI_RESTORING:
        appendf(buf, size, &used, "Timed off: turning Wi-Fi back on\n");
        break;
    }

    if (force_interval_seconds > 0)
        appendf(buf, size, &used, "Force: every %us\n",
            force_interval_seconds);
    else
        appendf(buf, size, &used, "Force: off\n");

    if (last_result < 0)
        appendf(buf, size, &used, "Last Wi-Fi request: error 0x%08X\n",
            last_result);
}
