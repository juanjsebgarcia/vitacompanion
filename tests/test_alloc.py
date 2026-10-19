import unittest

from test_command_features import compile_and_run


# Stand-in for VitaSDK's taipool.h, which is not available on the host.
TAIPOOL_HEADER = """
    #include <stddef.h>
    void *taipool_alloc(size_t size);
    void *taipool_calloc(size_t num, size_t size);
    void *taipool_realloc(void *ptr, size_t size);
    void taipool_free(void *ptr);
"""

# The host's own allocator keeps its names; the module's are renamed so
# both can live in one test binary.
RENAME = (
    "-Dmalloc=vc_malloc",
    "-Dcalloc=vc_calloc",
    "-Drealloc=vc_realloc",
    "-Dfree=vc_free",
)


class AllocTests(unittest.TestCase):
    def test_wrappers_keep_taipool_blocks_aligned(self):
        compile_and_run(
            r"""
            #include <assert.h>
            #include <stdint.h>
            #include <string.h>
            #include <taipool.h>

            void *vc_malloc(size_t size);
            void *vc_calloc(size_t num, size_t size);
            void *vc_realloc(void *ptr, size_t size);
            void vc_free(void *ptr);

            /* Mirrors taipool's layout: an 8-byte header (in-use flag, then
             * the size at +4) followed by exactly the requested bytes, with
             * the next block straight after. Freed blocks are not reused,
             * which is enough to show where each block lands. */
            static union { uint64_t align; unsigned char bytes[4096]; } arena;
            #define pool arena.bytes
            static size_t pool_used;
            static size_t last_request;
            static int frees;

            void *taipool_alloc(size_t size)
            {
                unsigned char *block = pool + pool_used;

                last_request = size;
                if (size > sizeof(pool) - pool_used - 8)
                    return NULL;
                block[0] = 1;
                memcpy(block + 4, &(uint32_t){(uint32_t)size}, 4);
                pool_used += 8 + size;
                return block + 8;
            }

            void taipool_free(void *ptr)
            {
                ((unsigned char *)ptr)[-8] = 0;
                frees++;
            }

            /* The wrappers must not use these: they deadlock on taipool's
             * semaphore. */
            void *taipool_calloc(size_t num, size_t size)
            {
                (void)num;
                (void)size;
                assert(!"taipool_calloc called");
                return NULL;
            }

            void *taipool_realloc(void *ptr, size_t size)
            {
                (void)ptr;
                (void)size;
                assert(!"taipool_realloc called");
                return NULL;
            }

            static int aligned(const void *ptr)
            {
                return ((uintptr_t)ptr & 7) == 0;
            }

            int main(void)
            {
                /* Without rounding, one odd block misaligns the next. */
                assert(aligned(taipool_alloc(5)));
                assert(!aligned(taipool_alloc(8)));
                pool_used = 0;

                /* Odd sizes, as promote's param.sfo read produces, are
                 * rounded up so every later block stays aligned. */
                for (size_t size = 0; size < 40; size++)
                {
                    unsigned char *p = vc_malloc(size);
                    assert(p != NULL);
                    assert(aligned(p));
                    assert(last_request % 8 == 0);
                    assert(last_request >= size && last_request < size + 8);
                    memset(p, 0xAA, size);
                }
                assert(pool_used % 8 == 0);

                /* Sizes that would wrap when rounded fail cleanly. */
                last_request = 0;
                assert(vc_malloc(SIZE_MAX) == NULL);
                assert(vc_malloc(SIZE_MAX - 6) == NULL);
                assert(last_request == 0);

                /* calloc zeroes, rejects num * size overflow, stays aligned. */
                memset(pool + pool_used, 0xFF, sizeof(pool) - pool_used);
                unsigned char *z = vc_calloc(3, 7);
                assert(z != NULL && aligned(z));
                assert(last_request == 24);
                for (int i = 0; i < 21; i++)
                    assert(z[i] == 0);
                last_request = 0;
                assert(vc_calloc(SIZE_MAX / 2 + 1, 2) == NULL);
                assert(vc_calloc(2, SIZE_MAX) == NULL);
                assert(last_request == 0);
                assert(aligned(vc_calloc(0, 5)));

                /* realloc: NULL acts as malloc, a block that already fits is
                 * kept, growing copies the data, size 0 frees. */
                char *r = vc_realloc(NULL, 3);
                assert(r != NULL && aligned(r));
                memcpy(r, "abc", 3);
                assert(vc_realloc(r, 7) == r);
                assert(vc_realloc(r, 1) == r);
                vc_malloc(1);
                int frees_before = frees;
                char *grown = vc_realloc(r, 13);
                assert(grown != NULL && grown != r && aligned(grown));
                assert(memcmp(grown, "abc", 3) == 0);
                assert(frees == frees_before + 1);
                assert(vc_realloc(grown, 0) == NULL);
                assert(frees == frees_before + 2);

                /* A failed grow leaves the original block alone. */
                char *keep = vc_malloc(8);
                memcpy(keep, "keep", 5);
                frees_before = frees;
                assert(vc_realloc(keep, sizeof(pool)) == NULL);
                assert(frees == frees_before);
                assert(strcmp(keep, "keep") == 0);

                /* free(NULL) is a no-op, unlike taipool_free. */
                frees_before = frees;
                vc_free(NULL);
                assert(frees == frees_before);
                return 0;
            }
            """,
            ["src/alloc.c"],
            headers={"taipool.h": TAIPOOL_HEADER},
            extra_cflags=RENAME,
        )


if __name__ == "__main__":
    unittest.main()
