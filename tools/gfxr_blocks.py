# Walk a .gfxr file's blocks. Returns list of (ordinal, offset, type, api_call_id or None).
import struct, sys
def walk(path):
    f = open(path, 'rb'); data = f.read()
    fourcc, major, minor, nopt = struct.unpack_from('<4I', data, 0)
    off = 16 + 8 * nopt; out = []; n = 0
    while off + 12 <= len(data):
        size, btype = struct.unpack_from('<QI', data, off)
        api = struct.unpack_from('<I', data, off + 12)[0] if (btype & 0x7FFFFFFF) in (4, 6) else None
        out.append((n, off, btype, api)); n += 1
        off += 12 + size
    return data, out, off == len(data)
if __name__ == '__main__':
    data, blocks, clean = walk(sys.argv[1])
    print('blocks', len(blocks), 'clean end', clean)
    for want in map(int, sys.argv[2:]):
        for shift in range(-3, 4):
            o = [b for b in blocks if b[0] == want + shift]
            if o: print(want, 'shift', shift, 'type %#x api %s' % (o[0][2], hex(o[0][3]) if o[0][3] else None))
