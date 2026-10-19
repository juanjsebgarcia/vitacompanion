#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <taipool.h>

/*
 * stdlib allocator for the user module, backed by taipool.
 *
 * taipool lays blocks out as an 8-byte header followed by exactly the
 * requested size, so one odd-sized request misaligns every block after
 * it. ARM LDRD/STRD/LDM/STM on such memory faults, which froze SceShell.
 * Rounding every request up to 8 keeps all blocks 8-aligned, because the
 * pool base is the 256-byte-offset start of a kernel-allocated thread
 * stack and the header is 8 bytes.
 *
 * calloc and realloc avoid taipool_calloc/taipool_realloc: both take
 * taipool's semaphore and then call taipool_alloc, which waits on it again,
 * and taipool_realloc's in-place paths misjudge block sizes.
 */

#define ALLOC_ALIGN 8
/* taipool keeps the block's size in the 4 bytes just before the data. */
#define ALLOC_BLOCK_SIZE(ptr) (((const uint32_t *)(ptr))[-1])

static int round_size(size_t size, size_t *rounded)
{
    if (size > SIZE_MAX - (ALLOC_ALIGN - 1))
        return -1;
    *rounded = (size + (ALLOC_ALIGN - 1)) & ~(size_t)(ALLOC_ALIGN - 1);
    return 0;
}

void *malloc(size_t size)
{
    size_t rounded;

    if (round_size(size, &rounded) < 0)
        return NULL;
    return taipool_alloc(rounded);
}

void *calloc(size_t num, size_t size)
{
    void *ptr;

    if (size != 0 && num > SIZE_MAX / size)
        return NULL;
    ptr = malloc(num * size);
    if (ptr)
        memset(ptr, 0, num * size);
    return ptr;
}

void free(void *ptr)
{
    /* taipool_free does not accept NULL. */
    if (ptr)
        taipool_free(ptr);
}

void *realloc(void *ptr, size_t size)
{
    size_t rounded;
    size_t old_size;
    void *new_ptr;

    if (!ptr)
        return malloc(size);
    if (size == 0)
    {
        free(ptr);
        return NULL;
    }
    if (round_size(size, &rounded) < 0)
        return NULL;
    /* A block that is already big enough is kept as is. */
    old_size = ALLOC_BLOCK_SIZE(ptr);
    if (old_size >= rounded)
        return ptr;

    new_ptr = malloc(rounded);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}
