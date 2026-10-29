#!/usr/bin/env python3
"""Show or change the theatre bounds in a Shogun 2 campaign_maps/<map>/regions.esf.

The campaign radar (minimap) maps clicks and the camera rectangle linearly from the radar image onto the
theatre rectangle stored here (regions.esf -> theatres -> theatre: two vec2 nodes, min and max corner, in
campaign world units). If a mod enlarges the map and its radar image, these bounds must be changed to the
world rectangle the new radar image covers, or minimap clicks land in the wrong place.

Usage:
  theatre_bounds.py regions.esf                         show the theatres and their bounds
  theatre_bounds.py regions.esf -o out.esf MINX MINY MAXX MAXY [--theatre KEY]
                                                        write a copy with new bounds (all theatres, or one key)

The file is rewritten byte for byte except the eight floats, so nothing else in it changes.
"""
import struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esf


class Abcf(esf.Esf):
    """ABCF: ABCE-style records with ABCA-style indexed strings."""
    def __init__(self, data):
        self.d = data
        self.magic = struct.unpack_from('<I', data, 0)[0]
        if self.magic != 0xABCF:
            raise ValueError('not an ABCF file (magic %#x)' % self.magic)
        (self.unk1, self.unk2, self.nodes_off) = struct.unpack_from('<III', data, 4)
        self.abca = True
        self._read_tables()
        self.abca = False
        self.pos = 16
        self.root = self._node()

    def _node(self):
        t = self.d[self.pos]
        if t in (0x0e, 0x0f):
            off = self.pos; self.pos += 1
            i = struct.unpack_from('<I', self.d, self.pos)[0]; self.pos += 4
            val = (self.ustr if t == 0x0e else self.astr).get(i, '?str%d' % i)
            return esf.Node(t, kids=[], val=val, off=off)
        return super()._node()


def load(data):
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic == 0xABCF:
        return Abcf(data)          # as shipped by CA
    if magic == 0xABCA:
        return esf.Esf(data)       # as re-saved by ESF editors / RPFM
    raise ValueError('unsupported ESF variant %#x (expected ABCF or ABCA)' % magic)


def theatres(e):
    """Yield (key, min_node, max_node) for every theatre record under 'theatres' and 'theatres_and_region_keys'."""
    for top in ('theatres', 'theatres_and_region_keys'):
        blk = e.root.first(top)
        if blk is None:
            continue
        for item in blk.kids:
            th = item.first('theatre') if item.first('theatre') else item
            kids = th.kids
            if len(kids) < 3 or kids[0].t != 0x0e or kids[1].t != 0x0c or kids[2].t != 0x0c:
                continue
            yield top, kids[0].val, kids[1], kids[2]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); return 1
    path = args.pop(0)
    out = None; key = None; vals = []
    while args:
        a = args.pop(0)
        if a == '-o': out = args.pop(0)
        elif a == '--theatre': key = args.pop(0)
        else: vals.append(float(a))
    data = bytearray(open(path, 'rb').read())
    e = load(bytes(data))
    found = list(theatres(e))
    if not found:
        print('no theatre records found'); return 1
    for top, k, mn, mx in found:
        print('%-26s theatre %-12s min=(%g, %g) max=(%g, %g)  -> width %g height %g' % (
            top, k, mn.val[0], mn.val[1], mx.val[0], mx.val[1], mx.val[0] - mn.val[0], mx.val[1] - mn.val[1]))
    if not vals:
        return 0
    if len(vals) != 4 or out is None:
        print('need -o OUT and four numbers: MINX MINY MAXX MAXY'); return 1
    minx, miny, maxx, maxy = vals
    if not (minx < maxx and miny < maxy):
        print('min must be less than max'); return 1
    n = 0
    for top, k, mn, mx in found:
        if key is not None and k != key:
            continue
        struct.pack_into('<ff', data, mn.off + 1, minx, miny)
        struct.pack_into('<ff', data, mx.off + 1, maxx, maxy)
        n += 1
    open(out, 'wb').write(bytes(data))
    print('wrote %s: %d theatre record(s) set to min=(%g, %g) max=(%g, %g)' % (out, n, minx, miny, maxx, maxy))
    # verify
    e2 = load(bytes(data))
    for top, k, mn, mx in theatres(e2):
        print('  verify %-26s %-12s min=(%g, %g) max=(%g, %g)' % (top, k, mn.val[0], mn.val[1], mx.val[0], mx.val[1]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
