#include "apps.h"

#include "promote.h"

#include <psp2/appmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The syscall copies out at most 80 bytes and fails with
 * SCE_KERNEL_ERROR_NO_MEMORY for a larger list, so 20 IDs is the limit. */
#define APPS_MAX_RUNNING 20
#define APPS_PATH_CAPACITY 64
#define APPS_NAME_CAPACITY 64
#define APPS_LINE_CAPACITY 128

/* Installed apps, game cards, then built-in system apps. */
static const char *const app_roots[] = { "ux0:app", "gro0:app", "vs0:app" };

static void read_app_name(const char *title_id, char *name, size_t capacity)
{
    char path[APPS_PATH_CAPACITY];
    size_t index;
    char *cursor;
    int length;

    name[0] = '\0';
    for (index = 0; index < sizeof(app_roots) / sizeof(app_roots[0]); ++index)
    {
        length = snprintf(path, sizeof(path), "%s/%s/sce_sys/param.sfo",
            app_roots[index], title_id);
        if (length < 0 || (size_t)length >= sizeof(path))
            continue;
        if (promote_read_sfo_string(path, "TITLE", name, capacity) > 0)
            break;
        name[0] = '\0';
    }

    /* Some titles contain line breaks; keep each app on one line. */
    for (cursor = name; *cursor != '\0'; ++cursor)
    {
        if ((unsigned char)*cursor < 0x20)
            *cursor = ' ';
    }
}

static int append_line(char *buffer, size_t capacity, size_t *used,
    char *line, size_t line_capacity, int length)
{
    if (length < 0)
        return 0;
    if ((size_t)length >= line_capacity)
    {
        length = (int)line_capacity - 1;
        line[length - 1] = '\n';
        line[length] = '\0';
    }
    if ((size_t)length >= capacity - *used)
        return 0;
    memcpy(buffer + *used, line, (size_t)length + 1);
    *used += (size_t)length;
    return 1;
}

int apps_format_list(char *buffer, size_t capacity)
{
    SceInt32 app_ids[APPS_MAX_RUNNING];
    char line[APPS_LINE_CAPACITY];
    size_t used = 0;
    int count;
    int index;
    int length;

    if (!buffer || capacity == 0)
        return -1;
    buffer[0] = '\0';

    count = sceAppMgrGetRunningAppIdListForShell(app_ids, APPS_MAX_RUNNING);
    if (count < 0)
        return count;
    if (count > APPS_MAX_RUNNING)
        count = APPS_MAX_RUNNING;
    if (count == 0)
        return snprintf(buffer, capacity, "No apps running.\n");

    length = snprintf(line, sizeof(line), "%-9s  %-10s  %-10s  %s\n",
        "TITLEID", "APPID", "PID", "NAME");
    if (!append_line(buffer, capacity, &used, line, sizeof(line),
        length))
        return (int)used;

    for (index = 0; index < count; ++index)
    {
        char title_id[32] = "?";
        char name[APPS_NAME_CAPACITY] = "";
        SceUID pid = sceAppMgrGetProcessIdByAppIdForShell(app_ids[index]);

        if (pid >= 0 && sceAppMgrGetNameById(pid, title_id) >= 0)
            read_app_name(title_id, name, sizeof(name));
        else
            strcpy(title_id, "?");

        length = snprintf(line, sizeof(line), "%-9s  0x%08X  0x%08X  %s\n",
            title_id, (uint32_t)app_ids[index], (uint32_t)pid, name);
        if (!append_line(buffer, capacity, &used, line, sizeof(line),
            length))
            break;
    }

    return (int)used;
}
