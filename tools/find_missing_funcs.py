#!/usr/bin/env python3
# Usage: tools/find_missing_funcs.py <image.bin> generated/default [already-added.toml]
# image.bin = guest 0x82000000..0x82CE0000; dump it from a running build with gdb:
#   dump binary memory image.bin 0x182000000 0x182CE0000   (keep it OUT of the repo)
# Find guest functions referenced by pointer (data words or lis/addi pairs in code)
# that ReXGlue didn't register. Emits candidate manifest lines with computed ends.
import re, struct, sys, glob, bisect
img_path, gen_dir = sys.argv[1], sys.argv[2]
img = open(img_path, 'rb').read(); BASE = 0x82000000
W = struct.unpack('>%dI' % (len(img)//4), img[:len(img)//4*4])
u32 = lambda a: W[(a-BASE)//4]
T0, T1 = 0x82170000, 0x82170000 + 0x956FE4
entries = set(int(m, 16) for m in re.findall(r'\{\s*0x([0-9A-Fa-f]{8}),\s*\w+\s*\}', open(gen_dir+'/gears1_recomp_init.cpp').read()))
if len(sys.argv) > 3:  # manifest-added entries to disregard (scan against the baseline)
    entries -= set(int(l[2:10], 16) for l in open(sys.argv[3]) if l.startswith('0x'))
labels = set()
for f in glob.glob(gen_dir+'/gears1_recomp_recomp.*.cpp'):
    labels.update(int(m, 16) for m in re.findall(r'^loc_([0-9A-F]{8}):', open(f).read(), re.M))
# .pdata: (begin, packed) with FunctionLength (instructions) in bits 8..29
pdata = []
for a in range(0x82137200, 0x82137200 + 0x314B8, 8):
    b, p = u32(a), u32(a+4)
    if b: pdata.append((b, b + 4*((p >> 8) & 0x3FFFFF)))
pdata.sort(); pbeg = [b for b, _ in pdata]
def in_pdata_body(x):
    i = bisect.bisect_right(pbeg, x) - 1
    return i >= 0 and pdata[i][0] < x < pdata[i][1]
refs = {}
for a in list(range(0x82000600, 0x82000600+0x136B34, 4)) + list(range(0x82B00000, 0x82B00000+0x11B90C, 4)):
    p = u32(a)
    if T0 <= p < T1 and p % 4 == 0: refs.setdefault(p, set()).add('d%08X' % a)
for i in range((T0-BASE)//4, (T1-BASE)//4):
    w = W[i]
    if w >> 26 == 15 and (w >> 16) & 31 == 0:
        rd, hi = (w >> 21) & 31, w & 0xFFFF
        for j in range(i+1, min(i+12, len(W))):
            v = W[j]
            if v >> 26 == 14 and (v >> 16) & 31 == rd:
                t = ((hi << 16) + (v & 0xFFFF if v & 0xFFFF < 0x8000 else (v & 0xFFFF) - 0x10000)) & 0xFFFFFFFF
                if T0 <= t < T1 and t % 4 == 0: refs.setdefault(t, set()).add('c%08X' % (BASE+4*i))
                break
TERM = lambda w: w in (0x4E800020, 0x4E800420, 0) or ((w >> 26) == 18 and not (w & 1))
cands = sorted(t for t in refs if t not in entries and t not in labels and not in_pdata_body(t) and TERM(u32(t-4)) and u32(t) != 0
         and not (T0 <= u32(t) < T1 and T0 <= u32(t+4) < T1))  # inline jump table after bctr
print('# candidates:', len(cands), file=sys.stderr)
bounds = sorted(entries | set(pbeg) | set(cands))
for t in cands:
    nxt = bounds[bisect.bisect_right(bounds, t)]
    # trim trailing zero padding back to the last terminator
    e = nxt
    while e - 4 > t and u32(e-4) == 0: e -= 4
    r = refs[t]
    print('0x%08X = { name = "sub_%08X", end = 0x%08X }  # refs=%d %s gap=%d' % (t, t, e, len(r), sorted(r)[0], nxt - t))
