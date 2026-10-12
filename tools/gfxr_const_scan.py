#!/usr/bin/env python3
# Scan every draw of the first frame in a GFXR capture for suspicious vertex
# shader float constants (descriptor set 1 binding 1 = Xenos float constants).
# Usage: tools/gfxr_const_scan.py <capture.gfxr> <capture.json> <shader_dir from gfxrecon-extract>
# Flags rows with NaN/Inf or |x| > 1e6, and notes draws whose VS uses relative
# constant addressing (a0 - typically skinning).
import json, math, os, re, struct, subprocess, sys
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
from gfxr_blocks import walk
from gfxr_ubo import fills, L


def main():
    cap, js, shdir = sys.argv[1], sys.argv[2], sys.argv[3]
    d = json.load(open(js))
    calls = [o for o in d if isinstance(o, dict) and 'function' in o]
    present = next(o['index'] for o in calls if o['function']['name'] == 'vkQueuePresentKHR')
    pipe_vs, rel_vs = {}, {}
    for o in calls:
        f = o['function']; a = f.get('args', {})
        if f['name'] == 'vkCreateGraphicsPipelines':
            for ci, p in zip(L(a['pCreateInfos']), L(a['pPipelines'])):
                for s in L(ci['pStages']):
                    if s['stage'].endswith('VERTEX_BIT'): pipe_vs[p] = s['module']
    def is_rel(m):
        if m not in rel_vs:
            p = os.path.join(shdir, f'sh{m}')
            asm = subprocess.run(['spirv-dis', p], capture_output=True, text=True).stdout if os.path.exists(p) else ''
            rel_vs[m] = 'xe_var_address_register' in asm and re.search(r'xe_uniform_float_constants %int_0 %\d+\n', asm) is not None
        return rel_vs[m]
    mem_of = {}
    for o in calls:
        a = o['function'].get('args', {})
        if o['function']['name'] == 'vkBindBufferMemory': mem_of[a['buffer']] = (a['memory'], a['memoryOffset'])
    # submits: cb -> submit index (first submit after recording)
    submits = []
    for o in calls:
        if o['function']['name'] == 'vkQueueSubmit':
            cbs = [cb for s in L(o['function']['args'].get('pSubmits')) for cb in L(s.get('pCommandBuffers'))]
            submits.append((o['index'], cbs))
    data, blocks, _ = walk(cap)
    snaps = {}  # mem -> list of (block, offset, raw)
    def snapshot(mem, before):
        if mem not in snaps: snaps[mem] = list(fills(data, blocks, mem))
        best = None
        for n, off, raw in snaps[mem]:
            if n < before: best = (n, off, raw)
        return best
    bound, pipe, writes = {}, {}, {}
    flagged = 0; total = 0; skinned = 0
    for o in calls:
        if o['index'] > present: break
        f = o['function']; a = f.get('args', {}); n = f['name']; cb = a.get('commandBuffer')
        if n == 'vkUpdateDescriptorSets':
            for w in L(a.get('pDescriptorWrites')):
                if w['descriptorType'].endswith('UNIFORM_BUFFER'): writes[(w['dstSet'], w['dstBinding'])] = L(w['pBufferInfo'])[0]
        elif n == 'vkCmdBindPipeline' and a['pipelineBindPoint'].endswith('GRAPHICS'): pipe[cb] = a['pipeline']
        elif n == 'vkCmdBindDescriptorSets' and a['pipelineBindPoint'].endswith('GRAPHICS'):
            for k, s in enumerate(L(a['pDescriptorSets'])): bound[(cb, a['firstSet'] + k)] = s
        elif n.startswith('vkCmdDraw'):
            total += 1
            vs = pipe_vs.get(pipe.get(cb)); rel = vs is not None and is_rel(vs); skinned += rel
            bi = writes.get((bound.get((cb, 1)), 1))
            sub = next((si for si, cbs in submits if si > o['index'] and cb in cbs), None)
            if not bi or sub is None: continue
            mem, moff = mem_of[bi['buffer']]; snap = snapshot(mem, sub)
            if not snap: continue
            _, off, raw = snap; start = moff + bi['offset'] - off
            blob = raw[start:start + bi['range']]
            bad = []
            for r in range(len(blob) // 16):
                v = struct.unpack_from('<4f', blob, r * 16)
                if any(math.isnan(x) or math.isinf(x) or abs(x) > 1e6 for x in v): bad.append((r, v))
            if bad:
                flagged += 1
                print(f'draw {o["index"]} {n} count {a.get("indexCount") or a.get("vertexCount")} pipe {pipe.get(cb)} vs {vs} rel={rel}: {len(bad)} bad rows, e.g. ' +
                      '; '.join('c%d=(%.3g %.3g %.3g %.3g)' % ((r,) + v) for r, v in bad[:3]))
    print(f'draws {total}, relative-addressing VS draws {skinned}, draws with bad float constants {flagged}')


if __name__ == '__main__':
    main()
