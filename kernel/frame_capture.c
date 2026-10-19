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
 * Swap the first and third channels, so the display's R,G,B,A byte order
 * comes out as B,G,R,X, which BMP stores as is. S3.9 fixed point.
 */
static SceIftuCscParams swap_red_blue_params = {
    0, 0, 0x3FF, 0, 0x3FF, 0,
    {
        {    0,     0, 0x200},
        {    0, 0x200,     0},
        {0x200,     0,     0}
    }
};

static SceUID capture_mutex = -1;
static SceUID capture_block = -1;
static uint8_t *capture_base;
static uint32_t capture_size;

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
    if (capture_block >= 0)
        ksceKernelFreeMemBlock(capture_block);
    capture_block = -1;
    capture_base = NULL;
    capture_size = 0;
}

static int get_displayed_frame(SceDisplayFrameBufInfo *info)
{
    int head = ksceDisplayGetPrimaryHead();
    int result;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    result = ksceDisplayGetProcFrameBufInternal(-1, head, 0, info);
    if (result < 0 || info->paddr == 0)
    {
        memset(info, 0, sizeof(*info));
        info->size = sizeof(*info);
        result = ksceDisplayGetProcFrameBufInternal(-1, head, 1, info);
    }
    if (result < 0)
        return result;
    if (info->paddr == 0 ||
        info->framebuf.width == 0 || info->framebuf.height == 0 ||
        info->framebuf.width > MAX_FRAME_WIDTH ||
        info->framebuf.height > MAX_FRAME_HEIGHT ||
        info->framebuf.pitch < ALIGN(info->framebuf.width, 16))
        return VITACOMPANION_KERNEL_ERROR_NO_FRAME;
    return 0;
}

static int allocate_capture(uint32_t size, uintptr_t *paddr)
{
    SceKernelAllocMemBlockKernelOpt opt;
    void *base;
    int result;

    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
        SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = 0x1000;
    capture_block = ksceKernelAllocMemBlock("vitacompanion_screenshot",
        SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW, size, &opt);
    /* Large DMA buffers may not fit in the ordinary kernel physical pool. */
    if (capture_block == NO_FREE_PHYSICAL_PAGE)
        capture_block = ksceKernelAllocMemBlock("vitacompanion_screenshot",
            SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW, size, &opt);
    if (capture_block < 0)
    {
        result = capture_block;
        capture_block = -1;
        return result;
    }

    result = ksceKernelGetMemBlockBase(capture_block, &base);
    if (result >= 0)
        result = ksceKernelGetPaddr(base, paddr);
    if (result < 0)
    {
        free_capture();
        return result;
    }

    capture_base = base;
    capture_size = size;
    return 0;
}

static int convert_frame(uintptr_t dst_paddr, unsigned int dst_pitch,
    const SceDisplayFrameBufInfo *info)
{
    unsigned int width = info->framebuf.width;
    unsigned int width_aligned = ALIGN(width, 16);
    unsigned int height = info->framebuf.height;
    unsigned int format = info->framebuf.pixelformat;
    SceIftuConvParams params;
    iftu_plane_state src;
    SceIftuFrameBuf dst;

    memset(&params, 0, sizeof(params));
    params.size = sizeof(params);
    params.unk04 = 1;
    params.csc_params1 = &swap_red_blue_params;
    params.csc_control = 1;
    params.alpha = 0xFF;

    memset(&src, 0, sizeof(src));
    src.fb.pixelformat = display_to_iftu_pixelformat(format);
    src.fb.width = width_aligned;
    src.fb.height = height;
    src.fb.leftover_stride = (info->framebuf.pitch - width_aligned) *
        display_pixelformat_bpp(format);
    src.fb.paddr0 = info->paddr;
    src.src_w = 0x10000;
    src.src_h = 0x10000;

    memset(&dst, 0, sizeof(dst));
    dst.pixelformat = SCE_IFTU_PIXELFORMAT_BGRX8888;
    dst.width = dst_pitch;
    dst.height = height;
    dst.paddr0 = dst_paddr;

    return ksceIftuCsc(&dst, (SceIftuPlaneState *)&src, &params);
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
    uintptr_t paddr;
    unsigned int pitch;
    int result;

    if (!info)
        return VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT;

    capture_lock();
    free_capture();

    /* Start just after a flip, so the frame stays on screen while copied. */
    ksceDisplayWaitVblankStart();
    result = get_displayed_frame(&frame);
    if (result < 0)
        goto out;

    pitch = ALIGN(frame.framebuf.width, 16);
    result = allocate_capture(
        ALIGN(pitch * ALIGN(frame.framebuf.height, 8) * 4, 0x1000), &paddr);
    if (result < 0)
        goto out;

    result = convert_frame(paddr, pitch, &frame);
    if (result < 0)
    {
        free_capture();
        goto out;
    }

    result_info.width = frame.framebuf.width;
    result_info.height = frame.framebuf.height;
    result_info.pitch = pitch;
    result_info.source_pixelformat = frame.framebuf.pixelformat;
    result = ksceKernelCopyToUser(info, &result_info, sizeof(result_info));
    if (result < 0)
        free_capture();

out:
    capture_unlock();
    return result < 0 ? result : 0;
}

int vitacompanion_screenshot_read(uint32_t offset, void *buffer,
    uint32_t size)
{
    int result;

    if (!buffer || size == 0)
        return VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT;

    capture_lock();
    if (!capture_base)
        result = VITACOMPANION_KERNEL_ERROR_NO_FRAME;
    else if (offset > capture_size || size > capture_size - offset)
        result = VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT;
    else
        result = ksceKernelCopyToUser(buffer, capture_base + offset, size);
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
