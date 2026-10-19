import importlib.util
import struct
import unittest
import zlib

from test_command_features import ROOT, compile_and_run


def load_tool():
    spec = importlib.util.spec_from_file_location(
        "screenshot_tool", ROOT / "tools" / "screenshot.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def decode_png_rgb(png):
    """Decode the filter-0, 8-bit RGB PNGs the tool writes."""
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    position = 8
    idat = b""
    while position < len(png):
        length, kind = struct.unpack_from(">I4s", png, position)
        data = png[position + 8:position + 8 + length]
        crc, = struct.unpack_from(">I", png, position + 8 + length)
        assert crc == zlib.crc32(kind + data) & 0xFFFFFFFF
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack_from(">IIBB", data)
            assert (depth, colour) == (8, 2)
        elif kind == b"IDAT":
            idat += data
        position += 12 + length
    raw = zlib.decompress(idat)
    stride = 1 + width * 3
    rows = []
    for y in range(height):
        assert raw[y * stride] == 0
        rows.append(raw[y * stride + 1:(y + 1) * stride])
    return width, height, rows


class ScreenshotToolTests(unittest.TestCase):
    def make_bmp(self, width, height, pixels, top_down=True, bpp=32):
        size = bpp // 8
        stride = (width * size + 3) & ~3
        body = b""
        order = range(height) if top_down else reversed(range(height))
        for y in order:
            row = b"".join(
                bytes((b, g, r)) + (b"\x00" if size == 4 else b"")
                for r, g, b in pixels[y])
            body += row + b"\x00" * (stride - len(row))
        header = struct.pack(
            "<2sIHHIIiiHHIIiiII", b"BM", 54 + len(body), 0, 0, 54, 40,
            width, -height if top_down else height, 1, bpp, 0,
            len(body), 2835, 2835, 0, 0)
        return header + body

    def test_converts_top_down_32_bit_bmp_to_png(self):
        tool = load_tool()
        pixels = [
            [(255, 0, 0), (0, 255, 0), (0, 0, 255)],
            [(1, 2, 3), (250, 128, 7), (255, 255, 255)],
        ]
        width, height, rows = tool.bmp_to_rgb_rows(
            self.make_bmp(3, 2, pixels))
        png = tool.png_bytes(width, height, rows)
        self.assertEqual(
            decode_png_rgb(png),
            (3, 2, [bytes(sum(row, ())) for row in pixels]))

    def test_converts_bottom_up_24_bit_bmp(self):
        tool = load_tool()
        pixels = [[(10, 20, 30)], [(40, 50, 60)]]
        _, _, rows = tool.bmp_to_rgb_rows(
            self.make_bmp(1, 2, pixels, top_down=False, bpp=24))
        self.assertEqual(rows, [bytes((10, 20, 30)), bytes((40, 50, 60))])

    def test_rejects_truncated_bmp(self):
        tool = load_tool()
        bmp = self.make_bmp(2, 2, [[(0, 0, 0)] * 2] * 2)
        with self.assertRaises(ValueError):
            tool.bmp_to_rgb_rows(bmp[:-1])


USER_HEADERS = {
    "psp2/io/fcntl.h": r"""
        #pragma once
        #include <stddef.h>
        typedef int SceUID;
        typedef unsigned int SceSize;
        #define SCE_O_WRONLY 2
        #define SCE_O_CREAT 0x200
        #define SCE_O_TRUNC 0x400
        SceUID sceIoOpen(const char *path, int flags, int mode);
        int sceIoWrite(SceUID fd, const void *data, SceSize size);
        int sceIoClose(SceUID fd);
        int sceIoRemove(const char *path);
        int sceIoRename(const char *from, const char *to);
    """,
    "psp2/io/stat.h": r"""
        #pragma once
        int sceIoMkdir(const char *path, int mode);
    """,
}

USER_STUBS = r"""
    #include "screenshot.h"

    #include <vitacompanion_kernel.h>
    #include <psp2/io/fcntl.h>
    #include <psp2/io/stat.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>

    static unsigned char file_data[4 << 20];
    static size_t file_size;
    static int file_open;
    static int fail_write_at = -1;
    static int write_calls;
    static int removed_tmp;
    static int renamed;
    static int released;
    static int capture_result;
    static int mkdir_result = (int)0x80010011;
    static vitacompanion_screenshot_info capture_info = {3, 2, 16, 0};

    SceUID sceIoOpen(const char *path, int flags, int mode)
    {
        (void)flags;
        (void)mode;
        if (strcmp(path, SCREENSHOT_PATH ".tmp") != 0)
            return -1;
        file_open = 1;
        file_size = 0;
        return 7;
    }

    int sceIoWrite(SceUID fd, const void *data, SceSize size)
    {
        if (fd != 7 || !file_open)
            return -1;
        if (write_calls++ == fail_write_at)
            return (int)0x80010005;
        memcpy(file_data + file_size, data, size);
        file_size += size;
        return (int)size;
    }

    int sceIoClose(SceUID fd)
    {
        file_open = 0;
        return fd == 7 ? 0 : -1;
    }

    int sceIoRemove(const char *path)
    {
        if (strcmp(path, SCREENSHOT_PATH ".tmp") == 0)
            ++removed_tmp;
        return 0;
    }

    int sceIoRename(const char *from, const char *to)
    {
        if (strcmp(from, SCREENSHOT_PATH ".tmp") != 0 ||
            strcmp(to, SCREENSHOT_PATH) != 0)
            return -1;
        ++renamed;
        return 0;
    }

    int sceIoMkdir(const char *path, int mode)
    {
        (void)mode;
        return strcmp(path, SCREENSHOT_DIRECTORY) == 0 ? mkdir_result : -1;
    }

    int vitaCompanionKernelScreenshotCapture(
        vitacompanion_screenshot_info *info)
    {
        if (capture_result < 0)
            return capture_result;
        *info = capture_info;
        return 0;
    }

    /* Pixel (x, y) of the capture is bytes {x, y, 0x80, 0xFF}. */
    int vitaCompanionKernelScreenshotRead(
        uint32_t offset, void *buffer, uint32_t size)
    {
        unsigned char *out = buffer;
        uint32_t total = capture_info.pitch * capture_info.height * 4;
        uint32_t i;

        if (offset > total || size > total - offset)
            return (int)0x80AC0002;
        for (i = 0; i < size; ++i)
        {
            uint32_t byte = offset + i;
            uint32_t pixel = byte / 4;
            uint32_t x = pixel % capture_info.pitch;
            uint32_t y = pixel / capture_info.pitch;
            static const unsigned char tail[2] = {0x80, 0xFF};
            out[i] = byte % 4 == 0 ? (unsigned char)x
                : byte % 4 == 1 ? (unsigned char)y : tail[byte % 4 - 2];
        }
        return 0;
    }

    int vitaCompanionKernelScreenshotRelease(void)
    {
        ++released;
        return 0;
    }

    static unsigned int le32(const unsigned char *p)
    {
        return p[0] | p[1] << 8 | p[2] << 16 | (unsigned int)p[3] << 24;
    }
"""


class ScreenshotUserTests(unittest.TestCase):
    def run_user(self, body):
        compile_and_run(
            USER_STUBS + "\nint main(void)\n{\n" + body + "\n}\n",
            ("src/screenshot.c",),
            headers=USER_HEADERS,
            extra_cflags=("-Wno-unused-variable", "-Wno-unused-function"),
        )

    def test_writes_trimmed_top_down_bmp(self):
        self.run_user(r"""
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t x;
            uint32_t y;

            if (screenshot_save(&width, &height) != 0)
                return 1;
            if (width != 3 || height != 2 || renamed != 1 || released != 1)
                return 2;
            if (file_size != 54 + 3 * 2 * 4 ||
                file_data[0] != 'B' || file_data[1] != 'M' ||
                le32(file_data + 2) != file_size ||
                le32(file_data + 10) != 54 ||
                le32(file_data + 14) != 40 ||
                le32(file_data + 18) != 3 ||
                (int)le32(file_data + 22) != -2 ||
                file_data[28] != 32 ||
                le32(file_data + 30) != 0 ||
                le32(file_data + 34) != 3 * 2 * 4)
                return 3;
            for (y = 0; y < 2; ++y)
                for (x = 0; x < 3; ++x)
                {
                    const unsigned char *p =
                        file_data + 54 + (y * 3 + x) * 4;
                    if (p[0] != x || p[1] != y || p[2] != 0x80)
                        return 4;
                }
            return 0;
        """)

    def test_failed_write_removes_temporary_file(self):
        self.run_user(r"""
            fail_write_at = 1;
            if (screenshot_save(0, 0) >= 0)
                return 1;
            if (renamed != 0 || removed_tmp != 1 || released != 1)
                return 2;
            return 0;
        """)

    def test_capture_error_is_returned(self):
        self.run_user(r"""
            capture_result = (int)0x80AC0003;
            if (screenshot_save(0, 0) != (int)0x80AC0003)
                return 1;
            if (renamed != 0 || write_calls != 0)
                return 2;
            return 0;
        """)

    def test_rejects_inconsistent_capture(self):
        self.run_user(r"""
            capture_info.pitch = 2;
            if (screenshot_save(0, 0) !=
                    VITACOMPANION_SCREENSHOT_ERROR_INVALID_FRAME)
                return 1;
            if (released != 1 || renamed != 0)
                return 2;
            return 0;
        """)

    def test_mkdir_failure_stops_before_capture(self):
        self.run_user(r"""
            mkdir_result = (int)0x80010002;
            if (screenshot_save(0, 0) != (int)0x80010002)
                return 1;
            if (released != 0)
                return 2;
            return 0;
        """)

    def test_large_frame_is_written_in_chunks(self):
        self.run_user(r"""
            capture_info.width = 960;
            capture_info.height = 544;
            capture_info.pitch = 960;
            if (screenshot_save(0, 0) != 0)
                return 1;
            if (file_size != 54 + 960 * 544 * 4 || write_calls < 32)
                return 2;
            return 0;
        """)


KERNEL_HEADERS = {
    "psp2kern/types.h": r"""
        #pragma once
        #include <stdint.h>
        typedef int SceUID;
        typedef unsigned int SceSize;
        typedef unsigned int SceUInt32;
    """,
    "psp2kern/display.h": r"""
        #pragma once
        #include <psp2kern/types.h>
        #define SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 0
        typedef struct {
            SceSize size;
            void *base;
            unsigned int pitch;
            unsigned int pixelformat;
            unsigned int width;
            unsigned int height;
        } SceDisplayFrameBuf;
        typedef struct {
            SceSize size;
            SceUID pid;
            unsigned int vblankcount;
            uintptr_t paddr;
            SceDisplayFrameBuf framebuf;
            unsigned int resolution;
        } SceDisplayFrameBufInfo;
        int ksceDisplayGetPrimaryHead(void);
        int ksceDisplayGetProcFrameBufInternal(SceUID pid, int head,
            int index, SceDisplayFrameBufInfo *info);
        int ksceDisplayWaitVblankStart(void);
    """,
    "psp2kern/kernel/sysmem.h": r"""
        #pragma once
        #include <psp2kern/types.h>
        #define SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW 0x10208006
        #define SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW 0x30808006
        #define SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT 0x4
        #define SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT 0x200000
        typedef struct {
            SceSize size;
            SceUInt32 field_4;
            SceUInt32 attr;
            SceUInt32 field_C;
            SceUInt32 paddr;
            SceSize alignment;
        } SceKernelAllocMemBlockKernelOpt;
        SceUID ksceKernelAllocMemBlock(const char *name, unsigned int type,
            SceSize size, SceKernelAllocMemBlockKernelOpt *opt);
        int ksceKernelFreeMemBlock(SceUID uid);
        int ksceKernelGetMemBlockBase(SceUID uid, void **base);
    """,
    "psp2kern/kernel/sysmem/address_space.h": r"""
        #pragma once
        #include <psp2kern/types.h>
        int ksceKernelVAtoPA(const void *va, uintptr_t *pa);
        #define ksceKernelGetPaddr(va, pa) ksceKernelVAtoPA(va, pa)
    """,
    "psp2kern/kernel/sysmem/data_transfers.h": r"""
        #pragma once
        #include <psp2kern/types.h>
        int ksceKernelCopyToUser(void *dst, const void *src, SceSize len);
    """,
    "psp2kern/kernel/threadmgr.h": r"""
        #pragma once
        #include <psp2kern/types.h>
        SceUID ksceKernelCreateMutex(const char *name, SceUInt32 attr,
            int count, void *opt);
        int ksceKernelDeleteMutex(SceUID id);
        int ksceKernelLockMutex(SceUID id, int count, unsigned int *timeout);
        int ksceKernelUnlockMutex(SceUID id, int count);
    """,
    "psp2kern/lowio/iftu.h": r"""
        #pragma once
        #define SCE_IFTU_PIXELFORMAT_BGRA5551 0x04
        #define SCE_IFTU_PIXELFORMAT_BGRX8888 0x10
        typedef struct {
            unsigned int post_add_0, post_add_1_2;
            unsigned int post_clamp_max_0, post_clamp_min_0;
            unsigned int post_clamp_max_1_2, post_clamp_min_1_2;
            unsigned int ctm[3][3];
        } SceIftuCscParams;
        typedef struct {
            unsigned int size, unk04;
            SceIftuCscParams *csc_params1, *csc_params2;
            unsigned int csc_control, unk14, unk18, unk1C, alpha, unk24;
        } SceIftuConvParams;
        typedef struct {
            unsigned int pixelformat, width, height;
            unsigned int leftover_stride, leftover_align;
            unsigned int paddr0, paddr1, paddr2;
        } SceIftuFrameBuf;
        typedef struct {
            SceIftuFrameBuf fb;
            unsigned int fields[13];
        } SceIftuPlaneState;
        int ksceIftuCsc(SceIftuFrameBuf *dst, SceIftuPlaneState *src,
            SceIftuConvParams *params);
    """,
}

KERNEL_STUBS = r"""
    #include "frame_capture.h"

    #include <psp2kern/display.h>
    #include <psp2kern/kernel/sysmem.h>
    #include <psp2kern/kernel/sysmem/address_space.h>
    #include <psp2kern/kernel/sysmem/data_transfers.h>
    #include <psp2kern/kernel/threadmgr.h>
    #include <psp2kern/lowio/iftu.h>
    #include <string.h>

    static SceDisplayFrameBufInfo frames[2];
    static int frame_results[2];
    static int lookups[2];
    static unsigned char block[4 << 20];
    static int alloc_results[2] = {0x1000, 0x2000};
    static unsigned int alloc_types[2];
    static SceSize alloc_size;
    static int allocs;
    static int frees;
    static int csc_result;
    static SceIftuFrameBuf csc_dst;
    static SceIftuFrameBuf csc_src;
    static unsigned int csc_scale[2];
    static int locked;

    int ksceDisplayGetPrimaryHead(void) { return 0; }
    int ksceDisplayWaitVblankStart(void) { return 0; }
    int ksceDisplayGetProcFrameBufInternal(SceUID pid, int head,
        int index, SceDisplayFrameBufInfo *info)
    {
        if (pid != -1 || head != 0)
            return -1;
        ++lookups[index];
        if (frame_results[index] < 0)
            return frame_results[index];
        *info = frames[index];
        return 0;
    }

    SceUID ksceKernelAllocMemBlock(const char *name, unsigned int type,
        SceSize size, SceKernelAllocMemBlockKernelOpt *opt)
    {
        (void)name;
        if (!(opt->attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT) ||
            size > sizeof(block))
            return -1;
        alloc_types[allocs] = type;
        alloc_size = size;
        return alloc_results[allocs++];
    }
    int ksceKernelFreeMemBlock(SceUID uid)
    {
        (void)uid;
        ++frees;
        return 0;
    }
    int ksceKernelGetMemBlockBase(SceUID uid, void **base)
    {
        (void)uid;
        *base = block;
        return 0;
    }
    int ksceKernelVAtoPA(const void *va, uintptr_t *pa)
    {
        *pa = va == block ? 0x60000000 : 0;
        return 0;
    }
    int ksceKernelCopyToUser(void *dst, const void *src, SceSize len)
    {
        memcpy(dst, src, len);
        return 0;
    }
    SceUID ksceKernelCreateMutex(const char *name, SceUInt32 attr,
        int count, void *opt)
    {
        (void)name; (void)attr; (void)count; (void)opt;
        return 5;
    }
    int ksceKernelDeleteMutex(SceUID id) { (void)id; return 0; }
    int ksceKernelLockMutex(SceUID id, int count, unsigned int *timeout)
    {
        (void)id; (void)count; (void)timeout;
        return locked++ == 0 ? 0 : -1;
    }
    int ksceKernelUnlockMutex(SceUID id, int count)
    {
        (void)id; (void)count;
        --locked;
        return 0;
    }
    int ksceIftuCsc(SceIftuFrameBuf *dst, SceIftuPlaneState *src,
        SceIftuConvParams *params)
    {
        csc_dst = *dst;
        csc_src = src->fb;
        csc_scale[0] = src->fields[3];
        csc_scale[1] = src->fields[4];
        if (params->alpha != 0xFF || params->csc_control != 1 ||
            params->csc_params1->ctm[0][0] != 0x200 ||
            params->csc_params1->ctm[0][2] != 0 ||
            params->csc_params1->ctm[2][2] != 0x200)
            return -1;
        block[0] = 0xAB;
        return csc_result;
    }

    static void set_frame(int index, unsigned int width, unsigned int height,
        unsigned int pitch, unsigned int format)
    {
        frames[index].paddr = 0x40000000 + index;
        frames[index].framebuf.width = width;
        frames[index].framebuf.height = height;
        frames[index].framebuf.pitch = pitch;
        frames[index].framebuf.pixelformat = format;
    }
"""


class ScreenshotKernelTests(unittest.TestCase):
    def run_kernel(self, body):
        compile_and_run(
            KERNEL_STUBS + "\nint main(void)\n{\n"
            "    vitacompanion_screenshot_info info;\n"
            "    unsigned char out[16];\n"
            "    if (vitacompanion_screenshot_init() < 0)\n"
            "        return 100;\n"
            + body + "\n}\n",
            ("kernel/frame_capture.c",),
            headers=KERNEL_HEADERS,
            include_dirs=("kernel",),
            extra_cflags=("-Wno-unused-variable", "-Wno-unused-function"),
        )

    def test_captures_primary_frame_at_native_size(self):
        self.run_kernel(r"""
            set_frame(0, 960, 544, 960, 0);
            if (vitacompanion_screenshot_capture(&info) != 0)
                return 1;
            if (info.width != 960 || info.height != 544 ||
                info.pitch != 960 || lookups[1] != 0)
                return 2;
            if (alloc_types[0] != 0x10208006 || alloc_size != 960 * 544 * 4)
                return 3;
            if (csc_src.paddr0 != 0x40000000 || csc_src.width != 960 ||
                csc_src.leftover_stride != 0 || csc_src.pixelformat != 0x10 ||
                csc_dst.paddr0 != 0x60000000 || csc_dst.width != 960 ||
                csc_dst.height != 544 || csc_dst.pixelformat != 0x10 ||
                csc_scale[0] != 0x10000 || csc_scale[1] != 0x10000)
                return 4;
            if (vitacompanion_screenshot_read(0, out, 1) != 0 ||
                out[0] != 0xAB || locked != 0)
                return 5;
            return 0;
        """)

    def test_falls_back_to_second_frame_and_maps_16_bit(self):
        self.run_kernel(r"""
            set_frame(0, 960, 544, 960, 0);
            frames[0].paddr = 0;
            set_frame(1, 720, 408, 768, 0x50000000);
            if (vitacompanion_screenshot_capture(&info) != 0)
                return 1;
            if (lookups[0] != 1 || lookups[1] != 1)
                return 2;
            if (info.width != 720 || info.pitch != 720 ||
                info.source_pixelformat != 0x50000000)
                return 3;
            if (csc_src.pixelformat != 0x04 ||
                csc_src.leftover_stride != (768 - 720) * 2 ||
                csc_src.paddr0 != 0x40000001)
                return 4;
            return 0;
        """)

    def test_unaligned_width_uses_aligned_pitch(self):
        self.run_kernel(r"""
            set_frame(0, 500, 300, 512, 0);
            if (vitacompanion_screenshot_capture(&info) != 0)
                return 1;
            if (info.width != 500 || info.pitch != 512 ||
                csc_dst.width != 512 || csc_src.leftover_stride != 0 ||
                alloc_size != 512 * 304 * 4)
                return 2;
            return 0;
        """)

    def test_no_frame_is_an_error(self):
        self.run_kernel(r"""
            frame_results[0] = -5;
            frame_results[1] = 0;
            if (vitacompanion_screenshot_capture(&info) !=
                    VITACOMPANION_KERNEL_ERROR_NO_FRAME)
                return 1;
            frame_results[1] = -6;
            if (vitacompanion_screenshot_capture(&info) != -6)
                return 2;
            set_frame(0, 960, 544, 512, 0);
            frame_results[0] = 0;
            if (vitacompanion_screenshot_capture(&info) !=
                    VITACOMPANION_KERNEL_ERROR_NO_FRAME)
                return 3;
            if (allocs != 0 || locked != 0)
                return 4;
            return 0;
        """)

    def test_falls_back_to_phycont_pool(self):
        self.run_kernel(r"""
            alloc_results[0] = (int)0x80024302;
            set_frame(0, 960, 544, 960, 0);
            if (vitacompanion_screenshot_capture(&info) != 0)
                return 1;
            if (allocs != 2 || alloc_types[1] != 0x30808006)
                return 2;
            return 0;
        """)

    def test_conversion_failure_frees_buffer(self):
        self.run_kernel(r"""
            set_frame(0, 960, 544, 960, 0);
            csc_result = (int)0x803F0704;
            if (vitacompanion_screenshot_capture(&info) != (int)0x803F0704)
                return 1;
            if (frees != 1)
                return 2;
            if (vitacompanion_screenshot_read(0, out, 1) !=
                    VITACOMPANION_KERNEL_ERROR_NO_FRAME)
                return 3;
            return 0;
        """)

    def test_read_bounds_and_release(self):
        self.run_kernel(r"""
            uint32_t size = 960 * 544 * 4;

            if (vitacompanion_screenshot_read(0, out, 1) !=
                    VITACOMPANION_KERNEL_ERROR_NO_FRAME)
                return 1;
            set_frame(0, 960, 544, 960, 0);
            if (vitacompanion_screenshot_capture(&info) != 0)
                return 2;
            if (vitacompanion_screenshot_read(size - 16, out, 16) != 0)
                return 3;
            if (vitacompanion_screenshot_read(size - 15, out, 16) !=
                    VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT ||
                vitacompanion_screenshot_read(size + 1, out, 0x10) !=
                    VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT ||
                vitacompanion_screenshot_read(16, out, 0xFFFFFFF8u) !=
                    VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT ||
                vitacompanion_screenshot_read(0, 0, 16) !=
                    VITACOMPANION_KERNEL_ERROR_INVALID_ARGUMENT)
                return 4;
            if (vitacompanion_screenshot_capture(&info) != 0 || frees != 1)
                return 5;
            vitacompanion_screenshot_release();
            if (frees != 2 ||
                vitacompanion_screenshot_read(0, out, 1) !=
                    VITACOMPANION_KERNEL_ERROR_NO_FRAME)
                return 6;
            vitacompanion_screenshot_term();
            if (frees != 2 || locked != 0)
                return 7;
            return 0;
        """)

    def test_exports_are_guarded_and_linked(self):
        source = (ROOT / "kernel" / "main.c").read_text()
        exports = (ROOT / "kernel" / "vitacompanion_kernel.yml").read_text()
        cmake = (ROOT / "CMakeLists.txt").read_text()
        for name in ("vitaCompanionKernelScreenshotCapture",
                     "vitaCompanionKernelScreenshotRead",
                     "vitaCompanionKernelScreenshotRelease"):
            self.assertIn("- " + name, exports)
            body = source.split("int " + name + "(", 1)[1].split("\n}", 1)[0]
            self.assertIn("caller_allowed()", body)
        self.assertIn("vitacompanion_screenshot_term();", source)
        for library in ("SceDisplayForDriver_stub", "SceIftuForDriver_stub",
                        "SceSysmemForDriver_stub"):
            self.assertIn(library, cmake)
        self.assertIn("kernel/frame_capture.c", cmake)
        self.assertIn("src/screenshot.c", cmake)


if __name__ == "__main__":
    unittest.main()
