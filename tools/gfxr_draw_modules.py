#!/usr/bin/env python3
"""Print the shader module ids (per stage) of the graphics pipeline bound for
each given draw block in a gfxrecon-convert JSON capture.
Usage: gfxr_draw_modules.py capture.json draw_block [draw_block...]"""
import json, sys
L = lambda x: x if isinstance(x, list) else ([] if x is None else [x])
d = json.load(open(sys.argv[1])); targets = set(int(x) for x in sys.argv[2:])
pipes, bound = {}, {}
for b in d:
    f = b.get('function')
    if not f: continue
    n, a = f['name'], f.get('args', {})
    if n == 'vkCreateGraphicsPipelines':
        for ci, h in zip(L(a['pCreateInfos']), L(a['pPipelines'])):
            pipes[h] = [(s['stage'].replace('VK_SHADER_STAGE_', '').replace('_BIT', ''), s['module']) for s in L(ci['pStages'])]
    elif n == 'vkCmdBindPipeline' and a['pipelineBindPoint'].endswith('GRAPHICS'):
        bound[a['commandBuffer']] = a['pipeline']
    elif n in ('vkCmdDraw', 'vkCmdDrawIndexed') and b['index'] in targets:
        p = bound.get(a['commandBuffer'])
        print(b['index'], 'pipeline', p, pipes.get(p))
