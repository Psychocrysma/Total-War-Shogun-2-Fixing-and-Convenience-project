#!/usr/bin/env python3
"""Compact ESF (ABCA / ABCE / ABCF) reader for Total War campaign files.

Node types:
  0x01 bool, 0x02 i8, 0x03 i16, 0x04 i32, 0x05 i64, 0x06 u8, 0x07 u16, 0x08 u32, 0x09 u64,
  0x0a f32, 0x0b f64, 0x0c vec2, 0x0d vec3, 0x0e utf16 string (ABCA: index into string table),
  0x0f ascii string (ABCA: index), 0x10 angle(u16)
  0x40.. arrays of the above (0x40 + type): u32 byte-size then raw items (ABCA: size is varint? no — ABCA arrays: for 0x40.. size prefixed)
  0x80 record: u16 name idx, u8 version, u32 end offset (ABCE); ABCA: compressed form
  0x81 record block (array of records)
This supports ABCE (Shogun 2 start positions after decompression use ABCA? In Shogun 2, startpos.esf is ABCE
 with a COMPRESSED_DATA node holding an LZMA stream whose payload is ABCA/ABCE).
"""
import struct, lzma, sys

class Node:
    __slots__ = ('t', 'name', 'ver', 'kids', 'val', 'off')
    def __init__(self, t, name=None, ver=None, kids=None, val=None, off=0):
        self.t, self.name, self.ver, self.kids, self.val, self.off = t, name, ver, kids, val, off
    def __repr__(self):
        if self.t in (0x80, 0x81):
            return f'<{self.name} v{self.ver} {len(self.kids)}>'
        return f'<{self.t:#x} {self.val!r}>'
    def find(self, name):
        return [k for k in self.kids if k.name == name]
    def first(self, name):
        for k in self.kids:
            if k.name == name:
                return k
        return None

SIZES = {1:1, 2:1, 3:2, 4:4, 5:8, 6:1, 7:2, 8:4, 9:8, 0xa:4, 0xb:8, 0xc:8, 0xd:12, 0x10:2}
FMT = {1:'<?', 2:'<b', 3:'<h', 4:'<i', 5:'<q', 6:'<B', 7:'<H', 8:'<I', 9:'<Q', 0xa:'<f', 0xb:'<d', 0xc:'<2f', 0xd:'<3f', 0x10:'<H'}

class Esf:
    def __init__(self, data):
        self.d = data
        magic = struct.unpack_from('<I', data, 0)[0]
        self.magic = magic
        if magic not in (0xABCE, 0xABCA, 0xABCF, 0xABCD):
            raise ValueError(f'bad magic {magic:#x}')
        self.abca = magic in (0xABCA, 0xABCF)
        (self.unk1, self.unk2, self.nodes_off) = struct.unpack_from('<III', data, 4)
        # footer: node name table (and string tables for ABCA)
        self._read_tables()
        self.pos = 16
        self.root = self._node()

    def _read_tables(self):
        d = self.d; p = self.nodes_off
        n = struct.unpack_from('<H', d, p)[0]; p += 2
        self.names = []
        for _ in range(n):
            l = struct.unpack_from('<H', d, p)[0]; p += 2
            self.names.append(d[p:p+l].decode('latin1')); p += l
        self.ustr = {}; self.astr = {}
        if self.abca and p < len(d):
            n = struct.unpack_from('<I', d, p)[0]; p += 4
            for _ in range(n):
                l = struct.unpack_from('<H', d, p)[0]; p += 2
                s = d[p:p+2*l].decode('utf-16-le'); p += 2*l
                i = struct.unpack_from('<I', d, p)[0]; p += 4
                self.ustr[i] = s
            n = struct.unpack_from('<I', d, p)[0]; p += 4
            for _ in range(n):
                l = struct.unpack_from('<H', d, p)[0]; p += 2
                s = d[p:p+l].decode('latin1'); p += l
                i = struct.unpack_from('<I', d, p)[0]; p += 4
                self.astr[i] = s

    def _varint(self):
        r = 0
        while True:
            b = self.d[self.pos]; self.pos += 1
            r = (r << 7) | (b & 0x7f)
            if not b & 0x80:
                return r

    def _node(self):
        d = self.d; off = self.pos
        t = d[self.pos]; self.pos += 1
        is_root = (off == 16)
        if (not self.abca and t == 0x80) or (self.abca and (t & 0xc0) == 0x80):  # record
            if self.abca and not is_root and not (t & 0x20):
                # short record header: 100vvvvx, name index in next byte
                ver = (t >> 1) & 0x0f
                name_idx = ((t & 1) << 8) | d[self.pos]; self.pos += 1
                size = self._varint()
                end = self.pos + size
            else:
                name_idx = struct.unpack_from('<H', d, self.pos)[0]; self.pos += 2
                ver = d[self.pos]; self.pos += 1
                if self.abca:
                    size = self._varint(); end = self.pos + size
                else:
                    end = struct.unpack_from('<I', d, self.pos)[0]; self.pos += 4
            kids = []
            while self.pos < end:
                kids.append(self._node())
            self.pos = end
            return Node(0x80, self.names[name_idx], ver, kids, off=off)
        if (not self.abca and t == 0x81) or (self.abca and (t & 0xc0) == 0xc0):  # record block
            if self.abca and not (t & 0x20):
                ver = (t >> 1) & 0x0f
                name_idx = ((t & 1) << 8) | d[self.pos]; self.pos += 1
                size = self._varint(); count = self._varint(); end = self.pos + size
            else:
                name_idx = struct.unpack_from('<H', d, self.pos)[0]; self.pos += 2
                ver = d[self.pos]; self.pos += 1
                if self.abca:
                    size = self._varint(); count = self._varint(); end = self.pos + size
                else:
                    end = struct.unpack_from('<I', d, self.pos)[0]; self.pos += 4
                    count = struct.unpack_from('<I', d, self.pos)[0]; self.pos += 4
            items = []
            for _ in range(count):
                if self.abca:
                    sz = self._varint(); iend = self.pos + sz
                else:
                    iend = struct.unpack_from('<I', d, self.pos)[0]; self.pos += 4
                kids = []
                while self.pos < iend:
                    kids.append(self._node())
                self.pos = iend
                items.append(Node(0x80, self.names[name_idx], ver, kids))
            self.pos = end
            return Node(0x81, self.names[name_idx], ver, items, off=off)
        if t & 0x40:  # array
            bt = t & 0x3f
            if self.abca:
                size = self._varint()
            else:
                end = struct.unpack_from('<I', d, self.pos)[0]; self.pos += 4
                size = end - self.pos
            raw = d[self.pos:self.pos+size]; self.pos += size
            if bt in SIZES:
                n = size // SIZES[bt]
                val = [struct.unpack_from(FMT[bt], raw, i*SIZES[bt])[0] for i in range(n)]
                if bt in (0xc, 0xd):
                    val = [struct.unpack_from(FMT[bt], raw, i*SIZES[bt]) for i in range(n)]
            else:
                val = raw
            return Node(t, kids=[], val=val, off=off)
        # primitive
        if self.abca and t in (0x0e, 0x0f):
            i = struct.unpack_from('<I', d, self.pos)[0]; self.pos += 4
            val = (self.ustr if t == 0x0e else self.astr).get(i, f'?str{i}')
            return Node(t, kids=[], val=val, off=off)
        if t == 0x0e:
            l = struct.unpack_from('<H', d, self.pos)[0]; self.pos += 2
            val = d[self.pos:self.pos+2*l].decode('utf-16-le'); self.pos += 2*l
            return Node(t, kids=[], val=val, off=off)
        if t == 0x0f:
            l = struct.unpack_from('<H', d, self.pos)[0]; self.pos += 2
            val = d[self.pos:self.pos+l].decode('latin1'); self.pos += l
            return Node(t, kids=[], val=val, off=off)
        if self.abca and t in (0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d):
            # ABCA optimised primitives: 0x12 bool true, 0x13 bool false, 0x14 u32 zero, 0x15 u32 one, 0x16 u32 byte,
            # 0x17 u32 short, 0x18 u32 24bit, 0x19 i32 zero, 0x1a i32 byte, 0x1b i32 short, 0x1c i32 24bit, 0x1d f32 zero
            if t == 0x12: return Node(1, kids=[], val=True, off=off)
            if t == 0x13: return Node(1, kids=[], val=False, off=off)
            if t == 0x14: return Node(8, kids=[], val=0, off=off)
            if t == 0x15: return Node(8, kids=[], val=1, off=off)
            if t == 0x16: v = d[self.pos]; self.pos += 1; return Node(8, kids=[], val=v, off=off)
            if t == 0x17: v = struct.unpack_from('<H', d, self.pos)[0]; self.pos += 2; return Node(8, kids=[], val=v, off=off)
            if t == 0x18: v = int.from_bytes(d[self.pos:self.pos+3], 'little'); self.pos += 3; return Node(8, kids=[], val=v, off=off)
            if t == 0x19: return Node(4, kids=[], val=0, off=off)
            if t == 0x1a: v = struct.unpack_from('<b', d, self.pos)[0]; self.pos += 1; return Node(4, kids=[], val=v, off=off)
            if t == 0x1b: v = struct.unpack_from('<h', d, self.pos)[0]; self.pos += 2; return Node(4, kids=[], val=v, off=off)
            if t == 0x1c: v = int.from_bytes(d[self.pos:self.pos+3], 'little', signed=True); self.pos += 3; return Node(4, kids=[], val=v, off=off)
            if t == 0x1d: return Node(0xa, kids=[], val=0.0, off=off)
        if t in SIZES:
            val = struct.unpack_from(FMT[t], d, self.pos); self.pos += SIZES[t]
            if t not in (0xc, 0xd): val = val[0]
            return Node(t, kids=[], val=val, off=off)
        raise ValueError(f'unknown node type {t:#x} at {off:#x}')

def decompress_startpos(data):
    """Shogun 2 startpos.esf: outer ABCE with COMPRESSED_DATA record containing LZMA raw stream + props."""
    e = Esf(data)
    cd = e.root.first('COMPRESSED_DATA')
    if cd is None:
        return e
    info = cd.first('COMPRESSED_DATA_INFO')
    raw = [k for k in cd.kids if k.t & 0x40][0].val
    size = [k for k in info.kids if not (k.t & 0x40)][0].val
    props = [k for k in info.kids if k.t & 0x40][0].val
    pb = bytes(props) if isinstance(props, list) else props
    lc = pb[0] % 9; rem = pb[0] // 9; lp = rem % 5; pbits = rem // 5
    dict_size = struct.unpack_from('<I', pb, 1)[0]
    filt = [{'id': lzma.FILTER_LZMA1, 'lc': lc, 'lp': lp, 'pb': pbits, 'dict_size': dict_size}]
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt)
    out = dec.decompress(bytes(raw), max_length=size)
    return Esf(out)

def walk(n, depth=0, maxdepth=3, out=print):
    if n.t in (0x80, 0x81):
        out('  '*depth + f'{n.name} (v{n.ver}, {len(n.kids)} {"items" if n.t==0x81 else "kids"})')
        if depth < maxdepth:
            for k in n.kids[:40]:
                walk(k, depth+1, maxdepth, out)
    else:
        v = n.val
        s = repr(v) if not isinstance(v, list) else f'array[{len(v)}] {v[:6]}'
        out('  '*depth + f'{n.t:#x}: {s[:120]}')

if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    e = decompress_startpos(data)
    walk(e.root, maxdepth=int(sys.argv[2]) if len(sys.argv) > 2 else 2)
