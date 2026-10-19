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

/* On the console, libk's snprintf ignored field widths, and lengths taken
 * from its return value hid every row after the header. Pad columns by hand
 * and take lengths from strlen. */
static void append_field(char *line, size_t capacity, size_t *used,
    const char *text, size_t width)
{
    size_t length = strlen(text);

    if (length > capacity - 1 - *used)
        length = capacity - 1 - *used;
    memcpy(line + *used, text, length);
    *used += length;
    while (length < width && *used < capacity - 1)
    {
        line[(*used)++] = ' ';
        ++length;
    }
    line[*used] = '\0';
}

static void format_line(char *line, size_t capacity, const char *title_id,
    const char *app_id, const char *pid, const char *name)
{
    size_t used = 0;

    /* Leave room for the newline. */
    append_field(line, capacity - 1, &used, title_id, 11);
    append_field(line, capacity - 1, &used, app_id, 12);
    append_field(line, capacity - 1, &used, pid, 12);
    append_field(line, capacity - 1, &used, name, 0);
    line[used++] = '\n';
    line[used] = '\0';
}

static int append_line(char *buffer, size_t capacity, size_t *used,
    const char *line)
{
    size_t length = strlen(line);

    if (length >= capacity - *used)
        return 0;
    memcpy(buffer + *used, line, length + 1);
    *used += length;
    return 1;
}

int apps_format_list(char *buffer, size_t capacity)
{
    SceInt32 app_ids[APPS_MAX_RUNNING];
    char line[APPS_LINE_CAPACITY];
    size_t used = 0;
    int count;
    int index;

    if (!buffer || capacity == 0)
        return -1;
    buffer[0] = '\0';

    count = sceAppMgrGetRunningAppIdListForShell(app_ids, APPS_MAX_RUNNING);
    if (count < 0)
        return count;
    if (count > APPS_MAX_RUNNING)
        count = APPS_MAX_RUNNING;
    if (count == 0)
    {
        append_line(buffer, capacity, &used, "No apps running.\n");
        return (int)used;
    }

    format_line(line, sizeof(line), "TITLEID", "APPID", "PID", "NAME");
    if (!append_line(buffer, capacity, &used, line))
        return (int)used;

    for (index = 0; index < count; ++index)
    {
        char title_id[32] = "?";
        char name[APPS_NAME_CAPACITY] = "";
        char app_id[16];
        char pid_text[16];
        SceUID pid = sceAppMgrGetProcessIdByAppIdForShell(app_ids[index]);

        if (pid >= 0 && sceAppMgrGetNameById(pid, title_id) >= 0)
            read_app_name(title_id, name, sizeof(name));
        else
            strcpy(title_id, "?");

        snprintf(app_id, sizeof(app_id), "0x%08X", (uint32_t)app_ids[index]);
        snprintf(pid_text, sizeof(pid_text), "0x%08X", (uint32_t)pid);
        format_line(line, sizeof(line), title_id, app_id, pid_text, name);
        if (!append_line(buffer, capacity, &used, line))
            break;
    }

    return (int)used;
}
