#!/usr/bin/env python3
"""Save the largest PNG-encoded icon of a Windows executable.

Usage:
    extract-icon.py <program.exe> <icon.png>

Exits with status 1 if the executable has no PNG icon.
"""

import struct
import sys
from pathlib import Path

RT_ICON = 3
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def section_offset(data: bytes, rva: int) -> int:
    """Translate a relative virtual address into a file offset."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections, optional_size = struct.unpack_from("<H12xH", data, pe + 6)
    table = pe + 24 + optional_size
    for i in range(sections):
        size, address, _, raw = struct.unpack_from("<4I", data, table + i * 40 + 8)
        if address <= rva < address + max(size, 1):
            return rva - address + raw
    raise ValueError(f"address {rva:#x} is in no section")


def entries(data: bytes, base: int, offset: int):
    """Yield (id, offset, is_directory) for one resource directory."""
    named, numbered = struct.unpack_from("<HH", data, base + offset + 12)
    for i in range(named + numbered):
        ident, target = struct.unpack_from("<II", data, base + offset + 16 + i * 8)
        yield ident, target & 0x7FFFFFFF, bool(target & 0x80000000)


def png_icons(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    directories = pe + 24 + (96 if magic == 0x10B else 112)
    resource_rva = struct.unpack_from("<I", data, directories + 2 * 8)[0]
    if not resource_rva:
        return
    base = section_offset(data, resource_rva)
    for kind, icons, _ in entries(data, base, 0):
        if kind != RT_ICON:
            continue
        for _, languages, _ in entries(data, base, icons):
            for _, leaf, is_directory in entries(data, base, languages):
                if is_directory:
                    continue
                rva, size = struct.unpack_from("<II", data, base + leaf)
                start = section_offset(data, rva)
                icon = data[start:start + size]
                if icon.startswith(PNG_SIGNATURE):
                    yield icon


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    icons = list(png_icons(Path(sys.argv[1]).read_bytes()))
    if not icons:
        print(f"No PNG icon in {sys.argv[1]}", file=sys.stderr)
        return 1
    Path(sys.argv[2]).write_bytes(max(icons, key=len))
    return 0


if __name__ == "__main__":
    sys.exit(main())
