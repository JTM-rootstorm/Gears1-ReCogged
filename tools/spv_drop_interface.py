#!/usr/bin/env python3
"""Remove variables from a SPIR-V module's OpEntryPoint interface list by
OpName (e.g. gl_SampleMask), leaving the declarations (now unused).
Usage: spv_drop_interface.py in.spv out.spv name [name...]"""
import struct, sys
src, dst, names = sys.argv[1], sys.argv[2], set(sys.argv[3:])
w = list(struct.unpack('<%dI' % (len(open(src, 'rb').read()) // 4), open(src, 'rb').read()))
ids, i = set(), 5
while i < len(w):
    n, op = w[i] >> 16, w[i] & 0xFFFF
    if op == 5:  # OpName
        s = struct.pack('<%dI' % (n - 2), *w[i + 2:i + n]).split(b'\0')[0].decode()
        if s in names: ids.add(w[i + 1])
    i += n
out, i = w[:5], 5
while i < len(w):
    n, op = w[i] >> 16, w[i] & 0xFFFF
    ins = w[i:i + n]
    if op == 15:  # OpEntryPoint: model, fn, name..., interface ids
        name_words = 0
        while True:
            name_words += 1
            if (ins[3 + name_words - 1] >> 24) == 0: break
        head = ins[:3 + name_words]
        iface = [x for x in ins[3 + name_words:] if x not in ids]
        ins = [((len(head) + len(iface)) << 16) | 15] + head[1:] + iface
    out += ins
    i += n
open(dst, 'wb').write(struct.pack('<%dI' % len(out), *out))
print('dropped ids', ids)
