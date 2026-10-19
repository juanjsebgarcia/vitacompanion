#!/usr/bin/env python3
"""Save a PNG of the Vita's screen.

Sends `screenshot` to the command server, fetches the BMP over FTP and
writes DIR/vita-YYYYmmdd-HHMMSS.png. Uses only the standard library.

    VITA_IP=10.0.0.90 tools/screenshot.py [DIR]

VITA_CMD_PORT (default 1338) and VITA_FTP_PORT (default 1337) select a
side-by-side test build.
"""

import ftplib
import io
import os
import socket
import struct
import sys
import time
import zlib

REMOTE_PATH = "ux0:/data/vitacompanion/screenshot.bmp"
COMMAND_TIMEOUT = 30


def send_command(host, port, command):
    with socket.create_connection((host, port), timeout=COMMAND_TIMEOUT) as s:
        s.sendall(command.encode() + b"\n")
        s.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            data = s.recv(4096)
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks).decode(errors="replace").strip()


def fetch(host, port, path):
    buffer = io.BytesIO()
    with ftplib.FTP() as ftp:
        ftp.connect(host, port, timeout=COMMAND_TIMEOUT)
        ftp.login()
        ftp.retrbinary("RETR " + path, buffer.write, blocksize=65536)
    return buffer.getvalue()


def bmp_to_rgb_rows(bmp):
    """Return (width, height, rows) from an uncompressed 24/32-bit BMP."""
    if bmp[:2] != b"BM" or len(bmp) < 54:
        raise ValueError("not a BMP file")
    offset, = struct.unpack_from("<I", bmp, 10)
    width, height, planes, bpp, compression = struct.unpack_from(
        "<iiHHI", bmp, 18)
    if planes != 1 or bpp not in (24, 32) or compression != 0 or width <= 0:
        raise ValueError("unsupported BMP layout")
    top_down = height < 0
    height = abs(height)
    pixel_size = bpp // 8
    stride = (width * pixel_size + 3) & ~3
    if offset + stride * height > len(bmp):
        raise ValueError("truncated BMP file")

    rows = []
    for y in range(height):
        source_row = y if top_down else height - 1 - y
        start = offset + source_row * stride
        row = bmp[start:start + width * pixel_size]
        rgb = bytearray(width * 3)
        rgb[0::3] = row[2::pixel_size]
        rgb[1::3] = row[1::pixel_size]
        rgb[2::3] = row[0::pixel_size]
        rows.append(bytes(rgb))
    return width, height, rows


def png_bytes(width, height, rows):
    def chunk(kind, data):
        body = kind + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                       8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) +
            chunk(b"IEND", b""))


def main(argv):
    if len(argv) > 2 or (len(argv) == 2 and argv[1] in ("-h", "--help")):
        print(__doc__.strip())
        return 0 if len(argv) == 2 else 2

    host = os.environ.get("VITA_IP")
    if not host:
        print("Error: set VITA_IP to the Vita's address.", file=sys.stderr)
        return 2
    cmd_port = int(os.environ.get("VITA_CMD_PORT", "1338"))
    ftp_port = int(os.environ.get("VITA_FTP_PORT", "1337"))
    directory = argv[1] if len(argv) == 2 else "screenshot"

    reply = send_command(host, cmd_port, "screenshot")
    if not reply.startswith("Saved "):
        print(reply or "Error: no reply from the Vita.", file=sys.stderr)
        return 1

    width, height, rows = bmp_to_rgb_rows(fetch(host, ftp_port, REMOTE_PATH))
    os.makedirs(directory, exist_ok=True)
    stem = os.path.join(directory, time.strftime("vita-%Y%m%d-%H%M%S"))
    path = stem + ".png"
    suffix = 1
    while os.path.exists(path):
        path = "%s-%d.png" % (stem, suffix)
        suffix += 1
    with open(path, "wb") as output:
        output.write(png_bytes(width, height, rows))
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
