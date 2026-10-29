#!/usr/bin/env python3
"""Minimal reader for Total War PFH3/PFH4 pack files (Shogun 2 uses PFH3).

Header (PFH3):
  0  char[4]  'PFH3'
  4  u32      pack type / flags  (bit 6 = index has timestamps? bits: 0..3 type)
  8  u32      pack_file_count   (dependency packs)
 12  u32      pack_file_index_size
 16  u32      file_count
 20  u32      file_index_size
 24  u32      timestamp (PFH3: 8 bytes? -> PFH3 header is 32 bytes)
Index entries: u32 size, [u64 timestamp if flag], then NUL-terminated path.
"""
import struct, sys, os

HAS_TS = 0x40

class Pack:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        hdr = self.f.read(32)
        magic = hdr[:4]
        if magic not in (b'PFH3', b'PFH2', b'PFH4', b'PFH0'):
            raise ValueError(f'not a PFH pack: {magic!r}')
        self.magic = magic
        (self.flags, self.dep_count, self.dep_index_size,
         self.file_count, self.file_index_size) = struct.unpack_from('<IIIII', hdr, 4)
        # PFH3/PFH2 header is 32 bytes (timestamp is 8 bytes); PFH0 28 bytes
        hdr_size = 32 if magic in (b'PFH3', b'PFH2', b'PFH4') else 28
        self.f.seek(hdr_size)
        dep_raw = self.f.read(self.dep_index_size)
        self.deps = [d.decode('latin1') for d in dep_raw.split(b'\x00') if d]
        idx_raw = self.f.read(self.file_index_size)
        self.entries = {}   # path -> (offset, size)
        self.order = []
        pos = 0
        data_start = hdr_size + self.dep_index_size + self.file_index_size
        off = data_start
        has_ts = bool(self.flags & HAS_TS)
        for _ in range(self.file_count):
            size = struct.unpack_from('<I', idx_raw, pos)[0]; pos += 4
            if has_ts:
                pos += 8   # 64-bit FILETIME per entry
            end = idx_raw.index(b'\x00', pos)
            name = idx_raw[pos:end].decode('latin1').replace('\\', '/')
            pos = end + 1
            self.entries[name] = (off, size)
            self.order.append(name)
            off += size

    def read(self, name):
        off, size = self.entries[name]
        self.f.seek(off)
        return self.f.read(size)

    def names(self, prefix=''):
        return [n for n in self.order if n.startswith(prefix)]

if __name__ == '__main__':
    p = Pack(sys.argv[1])
    print(p.magic, 'flags', hex(p.flags), 'deps', p.deps, 'files', p.file_count)
    pre = sys.argv[2] if len(sys.argv) > 2 else ''
    for n in p.names(pre):
        print(p.entries[n][1], n)
