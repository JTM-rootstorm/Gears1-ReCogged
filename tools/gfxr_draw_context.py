#!/usr/bin/env python3
"""Print the GFXR block indices needed to dump one draw with
gfxrecon-replay --dump-resources: its vkBeginCommandBuffer, the enclosing
render pass / dynamic rendering begin+end, and the vkQueueSubmit that runs it.
Usage: gfxr_draw_context.py capture.json draw_block [draw_block...]"""
import json, sys

d = json.load(open(sys.argv[1]))
targets = set(int(x) for x in sys.argv[2:])
by_index = {b['index']: b for b in d if 'index' in b}
begin_cb, rp_begin, out = {}, {}, {}
for b in d:
    f = b.get('function')
    if not f:
        continue
    n, a, i = f['name'], f.get('args', {}), b['index']
    cb = a.get('commandBuffer')
    if n == 'vkBeginCommandBuffer':
        begin_cb[cb] = i
    elif n in ('vkCmdBeginRenderPass', 'vkCmdBeginRendering', 'vkCmdBeginRenderingKHR'):
        rp_begin[cb] = i
    elif n in ('vkCmdEndRenderPass', 'vkCmdEndRendering', 'vkCmdEndRenderingKHR'):
        for t, o in out.items():
            if o['cb'] == cb and o.get('rp_end') is None and o['rp_begin'] == rp_begin.get(cb):
                o['rp_end'] = i
    elif n in ('vkCmdDraw', 'vkCmdDrawIndexed') and i in targets:
        out[i] = {'cb': cb, 'bcb': begin_cb.get(cb), 'rp_begin': rp_begin.get(cb), 'rp_end': None, 'submit': None}
    elif n == 'vkQueueSubmit':
        subs = a['pSubmits'] or []
        subs = [subs] if isinstance(subs, dict) else subs
        cbs = {c for s in subs for c in (s['pCommandBuffers'] or [])}
        for t, o in out.items():
            if o['submit'] is None and o['cb'] in cbs:
                o['submit'] = i
for t, o in sorted(out.items()):
    print(t, o)
