#include "screenshot.h"

#include <vitacompanion_kernel.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SCREENSHOT_TEMPORARY_PATH SCREENSHOT_PATH ".tmp"
#define SCREENSHOT_CHUNK_SIZE (64 * 1024)
#define PNG_IDAT_SIZE (64 * 1024)
#define SCE_ERROR_ERRNO_EEXIST ((int)0x80010011)
/* About 100 KB of deflate state, so it fits the plugin's memory pool. */
#define PNG_DEFLATE_LEVEL 1
#define PNG_DEFLATE_WINDOW_BITS 13
#define PNG_DEFLATE_MEMORY_LEVEL 7
/* Compress below the command thread's top priority, and give up if slow. */
#define SCREENSHOT_ENCODE_PRIORITY 0xA0
#define SCREENSHOT_ENCODE_LIMIT_US (5 * 1000 * 1000)
#define SCREENSHOT_LOG_PATH SCREENSHOT_DIRECTORY "/screenshot.log"

#ifdef VITACOMPANION_SCREENSHOT_DEBUG_LOG
/* Appends a line and closes the file, so the trail survives a freeze. */
static void debug_log(const char *format, ...)
{
    char line[160];
    va_list args;
    SceUID descriptor;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    descriptor = sceIoOpen(SCREENSHOT_LOG_PATH,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (descriptor < 0)
        return;
    sceIoWrite(descriptor, line, strlen(line));
    sceIoClose(descriptor);
}
#define DEBUG_LOG(...) debug_log(__VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#endif

/*
 * taipool does not round block sizes, so after any odd-sized allocation the
 * blocks that follow are misaligned, and zlib's state then faults on ARM.
 * Over-allocate and align, keeping the original pointer just below.
 */
#define ALLOCATION_ALIGNMENT 8

static void *aligned_malloc(size_t size)
{
    uint8_t *raw;
    uintptr_t aligned;

    if (size > (size_t)-1 - sizeof(void *) - ALLOCATION_ALIGNMENT)
        return NULL;
    raw = malloc(size + sizeof(void *) + ALLOCATION_ALIGNMENT - 1);
    if (!raw)
        return NULL;
    aligned = ((uintptr_t)raw + sizeof(void *) + ALLOCATION_ALIGNMENT - 1) &
        ~(uintptr_t)(ALLOCATION_ALIGNMENT - 1);
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

static void aligned_free(void *pointer)
{
    if (pointer)
        free(((void **)pointer)[-1]);
}

static voidpf zlib_alloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    if (size != 0 && items > (uInt)-1 / size)
        return Z_NULL;
    return aligned_malloc((size_t)items * size);
}

static void zlib_free(voidpf opaque, voidpf address)
{
    (void)opaque;
    aligned_free(address);
}

typedef struct {
    SceUID descriptor;
    z_stream stream;
    uint8_t *idat;
    screenshot_timings *timings;
    SceUInt64 started;
} png_writer;

static uint32_t elapsed_us(SceUInt64 start)
{
    return (uint32_t)(sceKernelGetProcessTimeWide() - start);
}

static void put_be32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static int write_all(png_writer *writer, const uint8_t *data, size_t size)
{
    SceUInt64 start = sceKernelGetProcessTimeWide();
    size_t written = 0;
    int result = 0;

    while (written < size)
    {
        result = sceIoWrite(writer->descriptor, data + written,
            (SceSize)(size - written));
        if (result < 0)
            break;
        if (result == 0 || (size_t)result > size - written)
        {
            result = VITACOMPANION_SCREENSHOT_ERROR_INCOMPLETE_IO;
            break;
        }
        written += (size_t)result;
        result = 0;
    }
    writer->timings->write_us += elapsed_us(start);
    return result;
}

static int write_chunk(png_writer *writer, const char *type,
    const uint8_t *data, uint32_t size)
{
    uint8_t header[8];
    uint8_t footer[4];
    uLong crc;
    int result;

    put_be32(header, size);
    memcpy(header + 4, type, 4);
    crc = crc32(0, header + 4, 4);
    if (size != 0)
        crc = crc32(crc, data, size);
    put_be32(footer, (uint32_t)crc);

    result = write_all(writer, header, sizeof(header));
    if (result >= 0 && size != 0)
        result = write_all(writer, data, size);
    if (result >= 0)
        result = write_all(writer, footer, sizeof(footer));
    return result;
}

static int write_header(png_writer *writer, uint32_t width, uint32_t height)
{
    static const uint8_t signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'
    };
    uint8_t ihdr[13];
    int result;

    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;    /* Bits per channel */
    ihdr[9] = 2;    /* RGB */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    result = write_all(writer, signature, sizeof(signature));
    if (result >= 0)
        result = write_chunk(writer, "IHDR", ihdr, sizeof(ihdr));
    return result;
}

/* Compresses input, writing an IDAT chunk each time the output fills. */
static int deflate_into_idat(png_writer *writer, const uint8_t *data,
    uint32_t size, int flush)
{
    writer->stream.next_in = (Bytef *)data;
    writer->stream.avail_in = size;

    for (;;)
    {
        SceUInt64 start = sceKernelGetProcessTimeWide();
        int status = deflate(&writer->stream, flush);
        uint32_t used;
        int result;

        writer->timings->encode_us += elapsed_us(start);
        if (status == Z_STREAM_ERROR)
        {
            DEBUG_LOG("deflate error %d\n", status);
            return VITACOMPANION_SCREENSHOT_ERROR_COMPRESSION;
        }

        used = PNG_IDAT_SIZE - writer->stream.avail_out;
        if (used == PNG_IDAT_SIZE || (status == Z_STREAM_END && used != 0))
        {
            DEBUG_LOG("idat %u bytes, in %u left\n",
                (unsigned int)used, (unsigned int)writer->stream.avail_in);
            result = write_chunk(writer, "IDAT", writer->idat, used);
            if (result < 0)
                return result;
            writer->stream.next_out = writer->idat;
            writer->stream.avail_out = PNG_IDAT_SIZE;
        }

        if (flush == Z_FINISH ? status == Z_STREAM_END
                              : writer->stream.avail_in == 0 &&
                                    writer->stream.avail_out != 0)
            return 0;
    }
}

/* Each PNG row is a filter byte (0, none) and R,G,B from the B,G,R,X capture. */
/*
 * The capture is B,G,R,X; frames the kernel flags as swapped hold R,G,B,X.
 * The swap is done here because IFTU ignored a channel-swap matrix for the
 * PSP emulator's padded frames.
 */
static void convert_row(uint8_t *row, const uint8_t *source, uint32_t width,
    int swapped_red_blue)
{
    unsigned int red = swapped_red_blue ? 0 : 2;
    unsigned int blue = 2 - red;
    uint32_t x;

    *row++ = 0;
    for (x = 0; x < width; ++x)
    {
        row[0] = source[red];
        row[1] = source[1];
        row[2] = source[blue];
        row += 3;
        source += 4;
    }
}

static int write_pixels(png_writer *writer,
    const vitacompanion_screenshot_info *info, uint8_t *chunk, uint8_t *row)
{
    uint32_t pitch_size = info->pitch * 4;
    uint32_t row_size = 1 + info->width * 3;
    uint32_t rows_per_chunk = SCREENSHOT_CHUNK_SIZE / pitch_size;
    uint32_t y = 0;

    while (y < info->height)
    {
        uint32_t rows = info->height - y;
        uint32_t index;
        SceUInt64 start;
        int result;

        if (rows > rows_per_chunk)
            rows = rows_per_chunk;
        if (elapsed_us(writer->started) > SCREENSHOT_ENCODE_LIMIT_US)
        {
            DEBUG_LOG("timed out at row %u\n", (unsigned int)y);
            return VITACOMPANION_SCREENSHOT_ERROR_TIMEOUT;
        }
        DEBUG_LOG("read rows %u-%u, %u us so far\n", (unsigned int)y,
            (unsigned int)(y + rows - 1),
            (unsigned int)elapsed_us(writer->started));
        start = sceKernelGetProcessTimeWide();
        result = vitaCompanionKernelScreenshotRead(
            y * pitch_size, chunk, rows * pitch_size);
        writer->timings->read_us += elapsed_us(start);
        if (result < 0)
        {
            DEBUG_LOG("read failed 0x%08X\n", (unsigned int)result);
            return result;
        }

        for (index = 0; index < rows; ++index)
        {
            start = sceKernelGetProcessTimeWide();
            convert_row(row, chunk + index * pitch_size, info->width,
                (int)info->swapped_red_blue);
            writer->timings->encode_us += elapsed_us(start);
            result = deflate_into_idat(writer, row, row_size, Z_NO_FLUSH);
            if (result < 0)
                return result;
        }
        y += rows;
        /* Let the shell's own threads run between chunks. */
        sceKernelDelayThread(0);
    }

    DEBUG_LOG("finishing\n");
    return deflate_into_idat(writer, NULL, 0, Z_FINISH);
}

static int write_png(const vitacompanion_screenshot_info *info,
    screenshot_timings *timings)
{
    png_writer writer;
    uint8_t *chunk;
    uint8_t *row;
    int result;
    int close_result;

    memset(&writer, 0, sizeof(writer));
    writer.timings = timings;
    writer.started = sceKernelGetProcessTimeWide();
    writer.stream.zalloc = zlib_alloc;
    writer.stream.zfree = zlib_free;
    chunk = aligned_malloc(SCREENSHOT_CHUNK_SIZE);
    row = aligned_malloc(1 + info->width * 3);
    writer.idat = aligned_malloc(PNG_IDAT_SIZE);
    if (!chunk || !row || !writer.idat ||
        deflateInit2(&writer.stream, PNG_DEFLATE_LEVEL, Z_DEFLATED,
            PNG_DEFLATE_WINDOW_BITS, PNG_DEFLATE_MEMORY_LEVEL,
            Z_DEFAULT_STRATEGY) != Z_OK)
    {
        aligned_free(chunk);
        aligned_free(row);
        aligned_free(writer.idat);
        return VITACOMPANION_SCREENSHOT_ERROR_NO_MEMORY;
    }
    writer.stream.next_out = writer.idat;
    writer.stream.avail_out = PNG_IDAT_SIZE;
    DEBUG_LOG("deflate ready, %ux%u pitch %u\n", (unsigned int)info->width,
        (unsigned int)info->height, (unsigned int)info->pitch);

    writer.descriptor = sceIoOpen(SCREENSHOT_TEMPORARY_PATH,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (writer.descriptor < 0)
        result = writer.descriptor;
    else
    {
        result = write_header(&writer, info->width, info->height);
        if (result >= 0)
            result = write_pixels(&writer, info, chunk, row);
        if (result >= 0)
            result = write_chunk(&writer, "IEND", NULL, 0);
        DEBUG_LOG("encoded, result 0x%08X\n", (unsigned int)result);

        close_result = sceIoClose(writer.descriptor);
        if (result >= 0 && close_result < 0)
            result = close_result;
    }

    deflateEnd(&writer.stream);
    aligned_free(chunk);
    aligned_free(row);
    aligned_free(writer.idat);

    if (result < 0)
    {
        if (writer.descriptor >= 0)
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

int screenshot_save(uint32_t buffer, vitacompanion_screenshot_info *info,
    screenshot_timings *timings)
{
    vitacompanion_screenshot_info local_info;
    screenshot_timings local_timings;
    SceUInt64 start = sceKernelGetProcessTimeWide();
    SceUInt64 capture_start;
    int result;

    if (!info)
        info = &local_info;
    if (!timings)
        timings = &local_timings;
    memset(info, 0, sizeof(*info));
    memset(timings, 0, sizeof(*timings));

    result = sceIoMkdir(SCREENSHOT_DIRECTORY, 0777);
    if (result < 0 && result != SCE_ERROR_ERRNO_EEXIST)
        return result;

    DEBUG_LOG("start\n");
    info->buffer = buffer;
    capture_start = sceKernelGetProcessTimeWide();
    result = vitaCompanionKernelScreenshotCapture(info);
    timings->capture_us = elapsed_us(capture_start);
    DEBUG_LOG("captured 0x%08X in %u us\n", (unsigned int)result,
        (unsigned int)timings->capture_us);
    if (result < 0)
        return result;

    if (info->width == 0 || info->height == 0 ||
        info->pitch < info->width ||
        info->pitch * 4 > SCREENSHOT_CHUNK_SIZE)
        result = VITACOMPANION_SCREENSHOT_ERROR_INVALID_FRAME;
    else
    {
        int priority = sceKernelGetThreadCurrentPriority();

        sceKernelChangeThreadPriority(0, SCREENSHOT_ENCODE_PRIORITY);
        result = write_png(info, timings);
        if (priority >= 0)
            sceKernelChangeThreadPriority(0, priority);
    }
    vitaCompanionKernelScreenshotRelease();
    timings->total_us = elapsed_us(start);
    DEBUG_LOG("done 0x%08X\n", (unsigned int)result);
    return result;
}
