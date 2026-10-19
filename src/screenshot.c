#include "screenshot.h"

#include <vitacompanion_kernel.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SCREENSHOT_TEMPORARY_PATH SCREENSHOT_PATH ".tmp"
#define SCREENSHOT_CHUNK_SIZE (64 * 1024)
#define BMP_HEADER_SIZE 54
#define SCE_ERROR_ERRNO_EEXIST ((int)0x80010011)

static void put_le16(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
    put_le16(destination, value);
    put_le16(destination + 2, value >> 16);
}

/* Top-down (negative height), uncompressed 32 bits per pixel. */
static void build_bmp_header(uint8_t header[BMP_HEADER_SIZE],
    uint32_t width, uint32_t height)
{
    uint32_t image_size = width * height * 4;

    memset(header, 0, BMP_HEADER_SIZE);
    header[0] = 'B';
    header[1] = 'M';
    put_le32(header + 2, BMP_HEADER_SIZE + image_size);
    put_le32(header + 10, BMP_HEADER_SIZE);
    put_le32(header + 14, 40);
    put_le32(header + 18, width);
    put_le32(header + 22, (uint32_t)-(int32_t)height);
    put_le16(header + 26, 1);
    put_le16(header + 28, 32);
    put_le32(header + 34, image_size);
    put_le32(header + 38, 2835);
    put_le32(header + 42, 2835);
}

static int write_all(SceUID descriptor, const uint8_t *data, size_t size)
{
    size_t written = 0;

    while (written < size)
    {
        int result = sceIoWrite(descriptor, data + written,
            (SceSize)(size - written));

        if (result < 0)
            return result;
        if (result == 0 || (size_t)result > size - written)
            return VITACOMPANION_SCREENSHOT_ERROR_INCOMPLETE_IO;
        written += (size_t)result;
    }
    return 0;
}

static int write_pixels(SceUID descriptor,
    const vitacompanion_screenshot_info *info, uint8_t *chunk)
{
    uint32_t row_size = info->width * 4;
    uint32_t pitch_size = info->pitch * 4;
    uint32_t rows_per_chunk = SCREENSHOT_CHUNK_SIZE / pitch_size;
    uint32_t row = 0;

    while (row < info->height)
    {
        uint32_t rows = info->height - row;
        uint32_t index;
        int result;

        if (rows > rows_per_chunk)
            rows = rows_per_chunk;
        result = vitaCompanionKernelScreenshotRead(
            row * pitch_size, chunk, rows * pitch_size);
        if (result < 0)
            return result;

        if (row_size == pitch_size)
            result = write_all(descriptor, chunk, rows * row_size);
        else
        {
            for (index = 0; index < rows && result >= 0; ++index)
                result = write_all(descriptor,
                    chunk + index * pitch_size, row_size);
        }
        if (result < 0)
            return result;
        row += rows;
    }
    return 0;
}

static int write_bmp(const vitacompanion_screenshot_info *info)
{
    uint8_t header[BMP_HEADER_SIZE];
    uint8_t *chunk;
    SceUID descriptor;
    int result;
    int close_result;

    chunk = malloc(SCREENSHOT_CHUNK_SIZE);
    if (!chunk)
        return VITACOMPANION_SCREENSHOT_ERROR_NO_MEMORY;

    descriptor = sceIoOpen(SCREENSHOT_TEMPORARY_PATH,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (descriptor < 0)
    {
        free(chunk);
        return descriptor;
    }

    build_bmp_header(header, info->width, info->height);
    result = write_all(descriptor, header, sizeof(header));
    if (result >= 0)
        result = write_pixels(descriptor, info, chunk);
    free(chunk);

    close_result = sceIoClose(descriptor);
    if (result >= 0 && close_result < 0)
        result = close_result;
    if (result < 0)
    {
        sceIoRemove(SCREENSHOT_TEMPORARY_PATH);
        return result;
    }

    /* Rename may not replace an existing file. */
    sceIoRemove(SCREENSHOT_PATH);
    result = sceIoRename(SCREENSHOT_TEMPORARY_PATH, SCREENSHOT_PATH);
    if (result < 0)
        sceIoRemove(SCREENSHOT_TEMPORARY_PATH);
    return result;
}

int screenshot_save(uint32_t *width, uint32_t *height)
{
    vitacompanion_screenshot_info info;
    int result;

    result = sceIoMkdir(SCREENSHOT_DIRECTORY, 0777);
    if (result < 0 && result != SCE_ERROR_ERRNO_EEXIST)
        return result;

    memset(&info, 0, sizeof(info));
    result = vitaCompanionKernelScreenshotCapture(&info);
    if (result < 0)
        return result;

    if (info.width == 0 || info.height == 0 || info.pitch < info.width ||
        info.pitch * 4 > SCREENSHOT_CHUNK_SIZE)
        result = VITACOMPANION_SCREENSHOT_ERROR_INVALID_FRAME;
    else
        result = write_bmp(&info);
    vitaCompanionKernelScreenshotRelease();

    if (result < 0)
        return result;
    if (width)
        *width = info.width;
    if (height)
        *height = info.height;
    return 0;
}
