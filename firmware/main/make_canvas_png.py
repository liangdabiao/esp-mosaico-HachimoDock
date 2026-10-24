#!/usr/bin/env python3
"""Generate the opaque 480x480 GSP canvas placeholder using only the stdlib."""
from pathlib import Path
import struct
import sys
import zlib


def chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))


def main() -> None:
    target = Path(sys.argv[1])
    width = height = 480
    row = b"\x00" + b"\x00\x00\x00" * width
    target.write_bytes(b"".join((
        b"\x89PNG\r\n\x1a\n",
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)),
        chunk(b"IDAT", zlib.compress(row * height, 9)),
        chunk(b"IEND", b""))))


if __name__ == "__main__":
    main()
