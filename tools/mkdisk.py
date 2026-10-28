#!/usr/bin/env python3
"""Create a raw disk image with a GPT holding one FAT32 data partition.

usage: mkdisk.py <image> <size-MiB> [label]

Used by `make disk` to give QEMU a SATA disk that ZenithOS mounts at /disk.
Needs mkfs.fat (dosfstools).
"""
import os
import struct
import subprocess
import sys
import uuid
import zlib

SECTOR = 512


def guid_bytes(s):
    return uuid.UUID(s).bytes_le


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path, size_mib = sys.argv[1], int(sys.argv[2])
    label = sys.argv[3] if len(sys.argv) > 3 else 'ZENITHDATA'
    total = size_mib * 1024 * 1024 // SECTOR
    first = 2048
    last_usable = total - 34
    last = (last_usable + 1) // 2048 * 2048 - 1

    with open(path, 'wb') as f:
        f.truncate(total * SECTOR)

    # protective MBR
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\x00\x02\x00', 0xEE, b'\xff\xff\xff', 1,
                               min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'

    entries = bytearray(128 * 128)
    name = 'ZenithOS data'.encode('utf-16-le')
    entries[0:128] = struct.pack('<16s16sQQQ72s', guid_bytes('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7'),
                                 uuid.uuid4().bytes_le, first, last, 0, name)
    ents_crc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4().bytes_le

    def header(my, alt, ents_lba):
        h = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, my, alt, 34,
                                  last_usable, disk_guid, ents_lba, 128, 128, ents_crc))
        struct.pack_into('<I', h, 16, zlib.crc32(h) & 0xFFFFFFFF)
        return bytes(h) + bytes(SECTOR - len(h))

    with open(path, 'r+b') as f:
        f.write(mbr)
        f.seek(1 * SECTOR); f.write(header(1, total - 1, 2))
        f.seek(2 * SECTOR); f.write(entries)
        f.seek((total - 33) * SECTOR); f.write(entries)
        f.seek((total - 1) * SECTOR); f.write(header(total - 1, 1, total - 33))

    blocks_kib = (last - first + 1) * SECTOR // 1024
    subprocess.run(['mkfs.fat', '-F', '32', '-n', label, '--offset', str(first), path, str(blocks_kib)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
