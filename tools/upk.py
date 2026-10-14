#!/usr/bin/env python3
"""Reader for the game's cooked UE3 packages (WarGame/CookedXenon/*.xxx,
package version 374, engine 2451, big-endian): summary, name/import/export
tables, LZO-compressed chunks (package compression flag 2) and whole-file
compressed packages (script/localisation packages; little-endian chunk
header, LZO blocks). Needs liblzo2 (dev-time only, loaded with ctypes).

Usage: tools/upk.py <package.xxx>   (export classes and shader caches)
"""
import ctypes, struct, sys

_lzo = ctypes.CDLL('liblzo2.so.2')
_lzo.__lzo_init_v2.restype = ctypes.c_int
assert _lzo.__lzo_init_v2(1, -1, -1, -1, -1, -1, -1, -1, -1, -1) == 0

def lzo_decompress(src, size):
    out = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(size)
    r = _lzo.lzo1x_decompress_safe(src, ctypes.c_size_t(len(src)), out, ctypes.byref(n), None)
    if r != 0 or n.value != size:
        raise ValueError(f'lzo error {r}, {n.value}/{size}')
    return out.raw

class Reader:
    def __init__(self, data, pos=0, endian='>'):
        self.d, self.p, self.e = data, pos, endian
    def u32(self):
        v = struct.unpack_from(self.e + 'I', self.d, self.p)[0]; self.p += 4; return v
    def i32(self):
        v = struct.unpack_from(self.e + 'i', self.d, self.p)[0]; self.p += 4; return v
    def u64(self):
        v = struct.unpack_from(self.e + 'Q', self.d, self.p)[0]; self.p += 8; return v
    def fstring(self):
        n = self.i32()
        if n < 0:  # UTF-16
            s = self.d[self.p:self.p - 2 * n].decode('utf-16-be' if self.e == '>' else 'utf-16-le'); self.p -= 2 * n
        else:
            s = self.d[self.p:self.p + n].decode('latin1'); self.p += n
        return s.rstrip('\0')

def decompress_chunk(d, offset):
    r = Reader(d, offset)
    tag, block = r.u32(), r.u32()
    assert tag == 0x9E2A83C1, hex(tag)
    total_c, total_u = r.u32(), r.u32()
    blocks = []
    left = total_u
    while left > 0:
        cs, us = r.u32(), r.u32(); blocks.append((cs, us)); left -= us
    out = bytearray()
    for cs, us in blocks:
        out += lzo_decompress(d[r.p:r.p + cs], us); r.p += cs
    return bytes(out)

def decompress_whole_file(d):
    """Whole-file compressed package (script/localisation packages): a
    little-endian chunk header and block table, LZO blocks, a normal
    big-endian package inside."""
    r = Reader(d, 0, '<')
    tag, _, total_c, total_u = r.u32(), r.u32(), r.u32(), r.u32()
    assert tag == 0x9E2A83C1
    blocks, left = [], total_u
    while left > 0:
        cs, us = r.u32(), r.u32(); blocks.append((cs, us)); left -= us
    out = bytearray()
    for cs, us in blocks:
        out += lzo_decompress(d[r.p:r.p + cs], us); r.p += cs
    return bytes(out)

class Package:
    def __init__(self, path):
        d = open(path, 'rb').read()
        if d[:4] == b'\xc1\x83\x2a\x9e':
            d = decompress_whole_file(d)
        r = Reader(d)
        assert r.u32() == 0x9E2A83C1
        self.version = r.u32() & 0xFFFF
        self.header_size = r.u32()
        self.folder = r.fstring()
        self.flags = r.u32()
        self.name_count, self.name_offset = r.u32(), r.u32()
        self.export_count, self.export_offset = r.u32(), r.u32()
        self.import_count, self.import_offset = r.u32(), r.u32()
        r.p += 16
        gens = r.u32(); r.p += gens * 12
        self.engine, self.cooker = r.u32(), r.u32()
        self.compression = r.u32()
        chunks = [(r.u32(), r.u32(), r.u32(), r.u32()) for _ in range(r.u32())]
        if chunks:
            # Uncompressed image: the header as stored, then each chunk at its offset.
            size = max(uo + us for uo, us, _, _ in chunks)
            img = bytearray(size)
            img[:self.header_size] = d[:self.header_size]
            for uo, us, co, cs in chunks:
                img[uo:uo + us] = decompress_chunk(d, co)
            d = bytes(img)
        self.data = d
        r = Reader(d, self.name_offset)
        self.names = []
        for _ in range(self.name_count):
            self.names.append(r.fstring()); r.u64()  # name flags
        r = Reader(d, self.import_offset)
        self.imports = []
        for _ in range(self.import_count):
            pkg, _, cls, _, outer, name, num = r.i32(), r.i32(), r.i32(), r.i32(), r.i32(), r.i32(), r.i32()
            self.imports.append((self.names[cls], self.names[name], outer))
        r = Reader(d, self.export_offset)
        self.exports = []
        for _ in range(self.export_count):
            cls, sup, outer, name, num = r.i32(), r.i32(), r.i32(), r.i32(), r.i32()
            arch = r.i32()
            oflags = r.u64()
            size, off = r.i32(), r.i32()
            ncomp = r.i32(); r.p += ncomp * 12  # component map
            eflags = r.u32()
            nnet = r.i32(); r.p += nnet * 4  # net objects per generation
            r.p += 16  # package guid
            self.exports.append((self.class_name(cls), self.names[name], size, off))
    def class_name(self, i):
        if i < 0: return self.imports[-i - 1][1]
        if i > 0: return self.exports[i - 1][1] if i - 1 < len(self.exports) else f'export{i}'
        return 'Class'

if __name__ == '__main__':
    p = Package(sys.argv[1])
    print(f'version {p.version} engine {p.engine} compression {p.compression} names {len(p.names)} '
          f'imports {len(p.imports)} exports {len(p.exports)} image {len(p.data):#x}')
    from collections import Counter
    for cls, n in Counter(e[0] for e in p.exports).most_common(12):
        print(f'  {n:5} {cls}')
    for e in p.exports:
        if 'Shader' in e[0]:
            print('  shader export:', e)
