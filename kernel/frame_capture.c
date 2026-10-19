/*
 * Displayed-frame capture. The frame lookup, pixel-format mapping, IFTU
 * plane setup and buffer allocation follow VitaStream
 * (devnoname120/vitastream) and vita-udcd-uvc (xerpi/vita_udcd_uvc).
 */
#include "frame_capture.h"

#include <psp2kern/display.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysmem/address_space.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/lowio/iftu.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define SCE_DISPLAY_PIXELFORMAT_BGRA5551 0x50000000
#define NO_FREE_PHYSICAL_PAGE ((int)0x80024302)
#define MAX_FRAME_WIDTH 1920
#define MAX_FRAME_HEIGHT 1088
/*
 * The capture is held in strips of this many rows, each its own small
 * physically contiguous block: a busy game can leave no single 2 MB block.
 */
#define STRIP_ROWS 32
#define MAX_STRIPS ((MAX_FRAME_HEIGHT + STRIP_ROWS - 1) / STRIP_ROWS)
#define NO_FREE_PHYSICAL_PAGE_UNIT ((int)0x80024303)

/* The SDK's SceIftuPlaneState names these fields differently. */
typedef struct {
    SceIftuFrameBuf fb;
    unsigned int reserved[3];
    unsigned int src_w;     /* Inverse scaling factor, 16.16 fixed point */
    unsigned int src_h;
    unsigned int dst_x;
    unsigned int dst_y;
    unsigned int src_x;     /* 8.8 fixed point */
    unsigned int src_y;
    unsigned int crop_top;
    unsigned int crop_bottom;
    unsigned int crop_left;
    unsigned int crop_right;
} iftu_plane_state;

typedef char iftu_plane_state_size[
    sizeof(iftu_plane_state) == 0x54 ? 1 : -1];

/*
 * Pass the channels through unchanged. On the console, A8B8G8R8 frames come
 * out as B,G,R,X, which BMP stores as is. S3.9 fixed point.
 */
static SceIftuCscParams identity_params = {
    0, 0, 0x3FF, 0, 0x3FF, 0,
    {
        {0x200,     0,     0},
        {    0, 0x200,     0},
        {    0,     0, 0x200}
    }
};

/*
 * Frames from the PSP emulator (which Adrenaline runs in) are labelled
 * A8B8G8R8 but hold R and B the other way round; the screen shows them
 * correctly. The capture flags them, and the user module swaps the channels.
 */
/* The emulator's display process, e.g. PSPEMU001 under Adrenaline. */
#define SWAPPED_OWNER_PREFIX "PSPEMU"

typedef struct {
    SceUID block;
    uint8_t *base;
    uintptr_t paddr;
} capture_strip;

static SceUID capture_mutex = -1;
static capture_strip strips[MAX_STRIPS];
static unsigned int strip_count;
static uint32_t strip_size;     /* Bytes of image per full strip */
static uint32_t capture_size;   /* Bytes of image in the whole capture */

static unsigned int display_to_iftu_pixelformat(unsigned int format)
{
    switch (format)
    {
    case SCE_DISPLAY_PIXELFORMAT_BGRA5551:
        return SCE_IFTU_PIXELFORMAT_BGRA5551;
    case SCE_DISPLAY_PIXELFORMAT_A8B8G8R8:
    default:
        return SCE_IFTU_PIXELFORMAT_BGRX8888;
    }
}

static unsigned int display_pixelformat_bpp(unsigned int format)
{
    switch (format)
    {
    case SCE_DISPLAY_PIXELFORMAT_BGRA5551:
        return 2;
    case SCE_DISPLAY_PIXELFORMAT_A8B8G8R8:
    default:
        return 4;
    }
}

static void capture_lock(void)
{
    if (capture_mutex >= 0)
        ksceKernelLockMutex(capture_mutex, 1, NULL);
}

static void capture_unlock(void)
{
    if (capture_mutex >= 0)
        ksceKernelUnlockMutex(capture_mutex, 1);
}

static void free_capture(void)
{
    unsigned int i;

    for (i = 0; i < strip_count; ++i)
    {
        if (strips[i].block >= 0)
            ksceKernelFreeMemBlock(strips[i].block);
    }
    memset(strips, 0, sizeof(strips));
    strip_count = 0;
    strip_size = 0;
    capture_size = 0;
}

static int query_frame(int head, int index, SceDisplayFrameBufInfo *info,
    vitacompanion_screenshot_buffer *report)
{
    int result;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    result = ksceDisplayGetProcFrameBufInternal(-1, head, index, info);
    report->result = result;
    report->pid = info->pid;
    report->paddr = info->paddr;
    report->pixelformat = info->framebuf.pixelformat;
    report->width = info->framebuf.width;
    report->height = info->framebuf.height;
    report->pitch = info->framebuf.pitch;
    memset(report->title_id, 0, sizeof(report->title_id));
    if (result >= 0 && info->pid > 0)
        ksceKernelSysrootGetProcessTitleId(info->pid, report->title_id,
            sizeof(report->title_id) - 1);
    return result;
}

static int owner_swaps_red_blue(const char *title_id)
{
    return strncmp(title_id, SWAPPED_OWNER_PREFIX,
        sizeof(SWAPPED_OWNER_PREFIX) - 1) == 0;
}

static int frame_is_usable(int result, const SceDisplayFrameBufInfo *info)
{
    return result >= 0 && info->paddr != 0 &&
        info->framebuf.width != 0 && info->framebuf.height != 0 &&
        info->framebuf.width <= MAX_FRAME_WIDTH &&
        info->framebuf.height <= MAX_FRAME_HEIGHT &&
        info->framebuf.pitch >= ALIGN(info->framebuf.width, 16);
}

/* Looks up both buffers, so callers can see what each one holds. */
static int get_displayed_frame(uint32_t request, SceDisplayFrameBufInfo *info,
    vitacompanion_screenshot_info *report, uint32_t *index)
{
    SceDisplayFrameBufInfo frames[2];
    int head = ksceDisplayGetPrimaryHead();
    int results[2];
    int chosen;

    results[0] = query_frame(head, 0, &frames[0], &report->buffers[0]);
    results[1] = query_frame(head, 1, &frames[1], &report->buffers[1]);

    if (request == VITACOMPANION_SCREENSHOT_BUFFER_0)
        chosen = 0;
    else if (request == VITACOMPANION_SCREENSHOT_BUFFER_1)
        chosen = 1;
    else
        chosen = results[0] >= 0 && frames[0].paddr != 0 ? 0 : 1;

    if (results[chosen] < 0)
        return results[chosen];
    if (!frame_is_usable(results[chosen], &frames[chosen]))
        return VITACOMPANION_KERNEL_ERROR_NO_FRAME;
    *info = frames[chosen];
    *index = (uint32_t)chosen;
    return 0;
}

static int allocate_strip(uint32_t size, capture_strip *strip)
{
    SceKernelAllocMemBlockKernelOpt opt;
    void *base;
    int result;

    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
        SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = 0x1000;
    strip->block = ksceKernelAllocMemBlock("vitacompanion_screenshot",
        SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW, size, &opt);
    /* DMA buffers may not fit in the ordinary kernel physical pool. */
    if (strip->block == NO_FREE_PHYSICAL_PAGE ||
        strip->block == NO_FREE_PHYSICAL_PAGE_UNIT)
        strip->block = ksceKernelAllocMemBlock("vitacompanion_screenshot",
            SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW, size, &opt);
    if (strip->block < 0)
    {
        result = strip->block;
        strip->block = -1;
        return result;
    }

    result = ksceKernelGetMemBlockBase(strip->block, &base);
    if (result >= 0)
        result = ksceKernelGetPaddr(base, &strip->paddr);
    if (result < 0)
        return result;
    strip->base = base;
    return 0;
}

static int allocate_capture(unsigned int pitch, unsigned int height)
{
    unsigned int count = (height + STRIP_ROWS - 1) / STRIP_ROWS;
    uint32_t block_size = ALIGN(pitch * STRIP_ROWS * 4, 0x1000);
    unsigned int i;
    int result;

    strip_size = pitch * STRIP_ROWS * 4;
    capture_size = pitch * height * 4;
    for (i = 0; i < count; ++i)
    {
        strip_count = i + 1;
        result = allocate_strip(block_size, &strips[i]);
        if (result < 0)
        {
            free_capture();
            return result;
        }
    }
    return 0;
}

/* Converts `height` rows starting at row `y` into one strip. */
static int convert_rows(uintptr_t dst_paddr, unsigned int dst_pitch,
    const SceDisplayFrameBufInfo *info, unsigned int y, unsigned int height)
{
    unsigned int width = info->framebuf.width;
    unsigned int width_aligned = ALIGN(width, 16);
    unsigned int format = info->framebuf.pixelformat;
    unsigned int bpp = display_pixelformat_bpp(format);
    SceIftuConvParams params;
    iftu_plane_state src;
    SceIftuFrameBuf dst;

    memset(&params, 0, sizeof(params));
    params.size = sizeof(params);
    params.unk04 = 1;
    params.csc_params1 = &identity_params;
    params.csc_control = 1;
    params.alpha = 0xFF;

    memset(&src, 0, sizeof(src));
    src.fb.pixelformat = display_to_iftu_pixelformat(format);
    src.fb.width = width_aligned;
    src.fb.height = height;
    src.fb.leftover_stride = (info->framebuf.pitch - width_aligned) * bpp;
    src.fb.paddr0 = info->paddr + y * info->framebuf.pitch * bpp;
    src.src_w = 0x10000;
    src.src_h = 0x10000;

    memset(&dst, 0, sizeof(dst));
    dst.pixelformat = SCE_IFTU_PIXELFORMAT_BGRX8888;
    dst.width = dst_pitch;
    dst.height = height;
    dst.paddr0 = dst_paddr;

    return ksceIftuCsc(&dst, (SceIftuPlaneState *)&src, &params);
}

/* Converts every strip back to back, so the frame is copied in one go. */
static int convert_frame(unsigned int pitch,
    const SceDisplayFrameBufInfo *info)
{
    unsigned int height = info->framebuf.height;
    unsigned int i;

    for (i = 0; i < strip_count; ++i)
    {
        unsigned int y = i * STRIP_ROWS;
        unsigned int rows = height - y < STRIP_ROWS ? height - y : STRIP_ROWS;
        int result = convert_rows(strips[i].paddr, pitch, info, y, rows);

        if (result < 0)
            return result;
    }
    return 0;
}

int vitacompanion_screenshot_init(void)
{
    capture_mutex = ksceKernelCreateMutex(
        "vitacompanion_screenshot_mutex", 0, 0, NULL);
    return capture_mutex < 0 ? capture_mutex : 0;
}

void vitacompanion_screenshot_term(void)
{
    capture_lock();
    free_capture();
    capture_unlock();
    if (capture_mutex >= 0)
        ksceKernelDeleteMutex(capture_mutex);
    capture_mutex = -1;
}

int vitacompanion_screenshot_capture(vitacompanion_screenshot_info *info)
{
    SceDisplayFrameBufInfo frame;
    vitacompanion_screenshot_info result_info;
    unsigned int pitch;
    uint32_t index = 0;
    int result;

    if (!info)
        return VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT;
    memset(&result_info, 0, sizeof(result_info));
    result = ksceKernelCopyFromUser(&result_info.buffer, &info->buffer,
        sizeof(result_info.buffer));
    if (result < 0)
        return result;
    if (result_info.buffer > VITACOMPANION_SCREENSHOT_BUFFER_1)
        return VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT;

    capture_lock();
    free_capture();

    /* Start just after a flip, so the frame stays on screen while copied. */
    ksceDisplayWaitVblankStart();
    result = get_displayed_frame(result_info.buffer, &frame, &result_info,
        &index);
    if (result < 0)
        goto report;

    pitch = ALIGN(frame.framebuf.width, 16);
    result = allocate_capture(pitch, frame.framebuf.height);
    if (result < 0)
        goto report;

    result_info.swapped_red_blue =
        owner_swaps_red_blue(result_info.buffers[index].title_id);
    result = convert_frame(pitch, &frame);
    if (result < 0)
    {
        free_capture();
        goto report;
    }

    result_info.buffer = index;
    result_info.width = frame.framebuf.width;
    result_info.height = frame.framebuf.height;
    result_info.pitch = pitch;
    result_info.source_pixelformat = frame.framebuf.pixelformat;

report:
    /* Report the buffers even on failure, for diagnosis. */
    {
        int copy_result = ksceKernelCopyToUser(
            info, &result_info, sizeof(result_info));

        if (result >= 0 && copy_result < 0)
        {
            free_capture();
            result = copy_result;
        }
    }
    capture_unlock();
    return result < 0 ? result : 0;
}

int vitacompanion_screenshot_read(uint32_t offset, void *buffer,
    uint32_t size)
{
    uint8_t *destination = buffer;
    int result = 0;

    if (!buffer || size == 0)
        return VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT;

    capture_lock();
    if (strip_count == 0)
        result = VITACOMPANION_KERNEL_ERROR_NO_FRAME;
    else if (offset > capture_size || size > capture_size - offset)
        result = VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT;
    while (result >= 0 && size != 0)
    {
        unsigned int index = offset / strip_size;
        uint32_t within = offset % strip_size;
        uint32_t length = strip_size - within;

        if (length > size)
            length = size;
        result = ksceKernelCopyToUser(destination,
            strips[index].base + within, length);
        destination += length;
        offset += length;
        size -= length;
    }
    capture_unlock();
    return result < 0 ? result : 0;
}

int vitacompanion_screenshot_release(void)
{
    capture_lock();
    free_capture();
    capture_unlock();
    return 0;
}
