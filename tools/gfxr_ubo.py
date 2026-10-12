#!/usr/bin/env python3
# Dump the uniform buffers a draw sees in a GFXR capture.
# Usage: [UBO_HEX=1] tools/gfxr_ubo.py <capture.gfxr> <capture.json from gfxrecon-convert> <draw_block_index> [set]
# For each UNIFORM_BUFFER binding of the bound descriptor set (default set 1),
# prints the buffer contents as float4 rows, decoded from the last FillMemory
# snapshot of its memory taken before the submit that runs the draw.
import json, os, struct, sys
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
from gfxr_blocks import walk

L = lambda x: x if isinstance(x, list) else ([] if x is None else [x])


def lz4_block(src, out_size):
    dst = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        dst += src[i:i + lit]; i += lit
        if i >= n: break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4; start = len(dst) - off
        if off >= ml:
            dst += dst[start:start + ml]
        else:
            for k in range(ml): dst.append(dst[start + k])
    assert len(dst) == out_size, (len(dst), out_size)
    return bytes(dst)


def fills(data, blocks, mem_id):
    """Yield (block_index, mapped_offset, bytes) for FillMemoryCommand blocks of mem_id."""
    for n, off, btype, api in blocks:
        if (btype & 0x7FFFFFFF) != 3: continue
        meta_id, tid, mid, moff, msize = struct.unpack_from('<IQQQQ', data, off + 12)
        if (meta_id & 0xFFFF) != 2 or mid != mem_id: continue
        size = struct.unpack_from('<Q', data, off)[0]
        payload = data[off + 48: off + 12 + size]
        yield n, moff, (lz4_block(payload, msize) if btype & 0x80000000 else bytes(payload))


def main():
    cap, js, draw = sys.argv[1], sys.argv[2], int(sys.argv[3])
    want_set = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    d = json.load(open(js))
    calls = [o for o in d if isinstance(o, dict) and 'function' in o]
    by_idx = {o['index']: o for o in calls}
    cb = by_idx[draw]['function']['args']['commandBuffer']
    # bound set at the draw (graphics bind point, latest bind covering want_set)
    bound = None
    for o in calls:
        if o['index'] >= draw: break
        f = o['function']; a = f.get('args', {})
        if f['name'] == 'vkCmdBindDescriptorSets' and a.get('commandBuffer') == cb and a['pipelineBindPoint'].endswith('GRAPHICS'):
            sets = L(a['pDescriptorSets']); first = a['firstSet']
            if first <= want_set < first + len(sets): bound = sets[want_set - first]
    # submit that runs the draw
    submit = next(o['index'] for o in calls if o['index'] > draw and o['function']['name'] == 'vkQueueSubmit'
                  and any(cb in L(s.get('pCommandBuffers')) for s in L(o['function']['args'].get('pSubmits'))))
    # latest descriptor writes for that set before the draw
    writes = {}
    for o in calls:
        if o['index'] >= draw: break
        f = o['function']
        if f['name'] == 'vkUpdateDescriptorSets':
            for w in L(f['args'].get('pDescriptorWrites')):
                if w.get('dstSet') == bound and w['descriptorType'].endswith('UNIFORM_BUFFER'):
                    writes[w['dstBinding']] = L(w['pBufferInfo'])[0]
    mem_of = {}
    for o in calls:
        f = o['function']; a = f.get('args', {})
        if f['name'] == 'vkBindBufferMemory': mem_of[a['buffer']] = (a['memory'], a['memoryOffset'])
    data, blocks, _ = walk(cap)
    print(f'draw {draw} cb {cb} set {want_set} = descriptor set {bound}, submit {submit}')
    for b in sorted(writes):
        bi = writes[b]; mem, moff = mem_of[bi['buffer']]
        snap = None
        for n, off, raw in fills(data, blocks, mem):
            if n < submit: snap = (n, off, raw)
        if not snap:
            print(f' b{b}: buffer {bi["buffer"]} mem {mem}: no snapshot'); continue
        n, off, raw = snap
        start = moff + bi['offset'] - off
        blob = raw[start:start + bi['range']]
        print(f' b{b}: buffer {bi["buffer"]} +{bi["offset"]:#x} range {bi["range"]} (mem {mem}, snapshot block {n})')
        for r in range(0, len(blob) // 16):
            v = struct.unpack_from('<4f', blob, r * 16)
            if os.environ.get('UBO_HEX'):
                u = struct.unpack_from('<4I', blob, r * 16)
                if any(u): print('   [%3d] %08X %08X %08X %08X' % ((r,) + u))
            elif any(v): print('   [%3d] % .5g % .5g % .5g % .5g' % ((r,) + v))


if __name__ == '__main__':
    main()
