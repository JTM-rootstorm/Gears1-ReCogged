#!/usr/bin/env python3
"""For every vkCmdDispatch in a block range, print the compute module and the
images (format/size/samples) of the sampled/storage image descriptors bound
to it - e.g. to see which host render target a Xenia EDRAM dump reads.
Usage: gfxr_dispatch_sources.py capture.json first_block last_block"""
import json, sys
L = lambda x: x if isinstance(x, list) else ([] if x is None else [x])
d = json.load(open(sys.argv[1])); lo, hi = int(sys.argv[2]), int(sys.argv[3])
images, views, sets, bound, pipes, cbound = {}, {}, {}, {}, {}, {}
for b in d:
    f = b.get('function')
    if not f: continue
    n, a = f['name'], f.get('args', {})
    if n == 'vkCreateImage':
        ci = a['pCreateInfo']; images[a['pImage']] = (ci['format'].replace('VK_FORMAT_', ''), ci['extent']['width'], ci['extent']['height'], ci['samples'].replace('VK_SAMPLE_COUNT_', ''))
    elif n == 'vkCreateImageView':
        ci = a['pCreateInfo']; views[a['pView']] = (ci['image'], ci['format'].replace('VK_FORMAT_', ''))
    elif n == 'vkCreateComputePipelines':
        for ci, h in zip(L(a['pCreateInfos']), L(a['pPipelines'])): pipes[h] = ci['stage']['module']
    elif n == 'vkUpdateDescriptorSets':
        for w in L(a.get('pDescriptorWrites')):
            for k, ii in enumerate(L(w.get('pImageInfo'))):
                if ii and ii.get('imageView'):
                    sets.setdefault(w['dstSet'], {})[(w['dstBinding'], w['dstArrayElement'] + k)] = ii['imageView']
    elif n == 'vkCmdBindPipeline' and a['pipelineBindPoint'].endswith('COMPUTE'):
        cbound[a['commandBuffer']] = a['pipeline']
    elif n == 'vkCmdBindDescriptorSets' and a['pipelineBindPoint'].endswith('COMPUTE'):
        for k, s in enumerate(L(a['pDescriptorSets'])):
            bound.setdefault(a['commandBuffer'], {})[a['firstSet'] + k] = s
    elif n == 'vkCmdDispatch' and lo <= b['index'] <= hi:
        cb = a['commandBuffer']
        srcs = []
        for si, s in sorted(bound.get(cb, {}).items()):
            for (bi, el), v in sorted(sets.get(s, {}).items()):
                img, vf = views.get(v, (None, '?'))
                srcs.append(f'set{si}.b{bi}: img {img} {vf} {images.get(img)}')
        print(b['index'], 'module', pipes.get(cbound.get(cb)), (a['groupCountX'], a['groupCountY']), '|', '; '.join(srcs) or 'no images')
