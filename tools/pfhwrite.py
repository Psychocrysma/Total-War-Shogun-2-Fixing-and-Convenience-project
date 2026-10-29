#!/usr/bin/env python3
"""Write a minimal Shogun 2 PFH3 pack. Usage: pfhwrite.py out.pack type dir
type: boot|release|patch|mod|movie (movie packs in data/ load without being listed in user.script)."""
import struct, sys, os
TYPES = {'boot': 0, 'release': 1, 'patch': 2, 'mod': 3, 'movie': 4}
def write(out, ptype, root):
    files = []
    for dp, dn, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root).replace('/', '\\')
            files.append((rel, open(p, 'rb').read()))
    files.sort()
    idx = b''.join(struct.pack('<I', len(d)) + rel.encode('latin1') + b'\0' for rel, d in files)
    hdr = b'PFH3' + struct.pack('<IIIII', TYPES[ptype], 0, 0, len(files), len(idx)) + struct.pack('<Q', 0)
    with open(out, 'wb') as o:
        o.write(hdr); o.write(idx)
        for rel, d in files: o.write(d)
    print('wrote', out, len(files), 'files')
if __name__ == '__main__':
    write(sys.argv[1], sys.argv[2], sys.argv[3])
