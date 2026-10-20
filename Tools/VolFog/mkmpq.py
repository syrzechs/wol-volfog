"""Minimal MPQ (format v1) writer for small interface patches of the WoW 3.3.5a client.

usage: python -I mkmpq.py <output.MPQ> <root_dir> <file inside root> [<file> ...]

Each file is stored under its path relative to <root_dir> (with backslashes, e.g. Interface\\GlueXML\\AccountLogin.lua),
zlib-compressed in 4096-byte sectors with a sector offset table, locale neutral, not encrypted. A (listfile) is added.
Hash and block tables are encrypted as the format requires. No external dependencies.
"""
import os
import struct
import sys
import zlib

MPQ_FILE_COMPRESS = 0x00000200
MPQ_FILE_EXISTS = 0x80000000
SECTOR_SHIFT = 3  # 512 << 3 = 4096


def _crypt_table():
    table = [0] * 0x500
    seed = 0x00100001
    for index1 in range(0x100):
        index2 = index1
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            temp1 = (seed & 0xFFFF) << 0x10
            seed = (seed * 125 + 3) % 0x2AAAAB
            temp2 = seed & 0xFFFF
            table[index2] = temp1 | temp2
            index2 += 0x100
    return table


CRYPT = _crypt_table()


def hash_string(name, hash_type):
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in name.replace('/', '\\').upper().encode('ascii'):
        value = CRYPT[(hash_type << 8) + ch]
        seed1 = (value ^ (seed1 + seed2)) & 0xFFFFFFFF
        seed2 = (ch + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def encrypt(data, key):
    assert len(data) % 4 == 0
    seed1, seed2 = key, 0xEEEEEEEE
    out = []
    for (value,) in struct.iter_unpack('<I', data):
        seed2 = (seed2 + CRYPT[0x400 + (seed1 & 0xFF)]) & 0xFFFFFFFF
        out.append((value ^ (seed1 + seed2)) & 0xFFFFFFFF)
        seed1 = (((~seed1 << 0x15) + 0x11111111) | (seed1 >> 0x0B)) & 0xFFFFFFFF
        seed2 = (value + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return struct.pack('<%dI' % len(out), *out)


def pack_file(data):
    sector_size = 512 << SECTOR_SHIFT
    sectors = [data[i:i + sector_size] for i in range(0, len(data), sector_size)] or [b'']
    blobs = []
    for sector in sectors:
        packed = b'\x02' + zlib.compress(sector, 9)  # 0x02 = zlib
        blobs.append(packed if len(packed) < len(sector) else sector)
    offsets = [(len(sectors) + 1) * 4]
    for blob in blobs:
        offsets.append(offsets[-1] + len(blob))
    return struct.pack('<%dI' % len(offsets), *offsets) + b''.join(blobs)


def build(output, root, files):
    entries = []
    for path in files:
        rel = os.path.relpath(path, root).replace('/', '\\')
        with open(path, 'rb') as f:
            entries.append((rel, f.read()))
    listfile = ''.join(name + '\r\n' for name, _ in entries).encode('ascii')
    entries.append(('(listfile)', listfile))

    hash_size = 16
    while hash_size < len(entries) * 2:
        hash_size *= 2

    body = bytearray()
    blocks = []
    header_size = 32
    for name, data in entries:
        packed = pack_file(data)
        blocks.append((header_size + len(body), len(packed), len(data), MPQ_FILE_EXISTS | MPQ_FILE_COMPRESS))
        body += packed

    hash_table = [(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)] * hash_size
    for block_index, (name, _) in enumerate(entries):
        index = hash_string(name, 0) & (hash_size - 1)
        while hash_table[index][4] != 0xFFFFFFFF:
            index = (index + 1) & (hash_size - 1)
        hash_table[index] = (hash_string(name, 1), hash_string(name, 2), 0, 0, block_index)

    hash_raw = b''.join(struct.pack('<IIHHI', *e) for e in hash_table)
    block_raw = b''.join(struct.pack('<IIII', *b) for b in blocks)
    hash_offset = header_size + len(body)
    block_offset = hash_offset + len(hash_raw)
    archive_size = block_offset + len(block_raw)

    header = struct.pack('<4sIIHHIIII', b'MPQ\x1a', header_size, archive_size, 0, SECTOR_SHIFT,
                         hash_offset, block_offset, hash_size, len(blocks))
    with open(output, 'wb') as f:
        f.write(header)
        f.write(body)
        f.write(encrypt(hash_raw, hash_string('(hash table)', 3)))
        f.write(encrypt(block_raw, hash_string('(block table)', 3)))
    for name, data in entries:
        print('  %-45s %6d bytes' % (name, len(data)))
    print('wrote %s (%d bytes)' % (output, archive_size))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], sys.argv[3:])
