#!/usr/bin/env python3
"""List the sampled images (format, size, image id) bound to a draw in a
gfxrecon-convert JSON capture, per descriptor set/binding.
Usage: gfxr_draw_textures.py capture.json draw_block"""
import json, sys
L = lambda x: x if isinstance(x, list) else ([] if x is None else [x])
d = json.load(open(sys.argv[1])); target = int(sys.argv[2])
images, views, sets, bound = {}, {}, {}, {}
for b in d:
    f = b.get('function')
    if not f: continue
    n, a = f['name'], f.get('args', {})
    if n == 'vkCreateImage':
        ci = a['pCreateInfo']; images[a['pImage']] = (ci['format'].replace('VK_FORMAT_', ''), ci['extent']['width'], ci['extent']['height'], ci['samples'].replace('VK_SAMPLE_COUNT_', ''))
    elif n == 'vkCreateImageView':
        ci = a['pCreateInfo']; views[a['pView']] = (ci['image'], ci['format'].replace('VK_FORMAT_', ''), ci.get('viewType', '').replace('VK_IMAGE_VIEW_TYPE_', ''))
    elif n == 'vkUpdateDescriptorSets':
        for w in L(a.get('pDescriptorWrites')):
            for k, ii in enumerate(L(w.get('pImageInfo'))):
                if ii and ii.get('imageView'):
                    sets.setdefault(w['dstSet'], {})[(w['dstBinding'], w['dstArrayElement'] + k)] = ii['imageView']
    elif n == 'vkCmdBindDescriptorSets' and a['pipelineBindPoint'].endswith('GRAPHICS'):
        for k, s in enumerate(L(a['pDescriptorSets'])):
            bound.setdefault(a['commandBuffer'], {})[a['firstSet'] + k] = s
    elif n in ('vkCmdDraw', 'vkCmdDrawIndexed') and b['index'] == target:
        for si, s in sorted(bound.get(a['commandBuffer'], {}).items()):
            for (bi, el), v in sorted(sets.get(s, {}).items()):
                img, vf, vt = views.get(v, (None, '?', '?'))
                print(f'set {si} binding {bi}[{el}]: view {v} {vf} {vt} -> image {img} {images.get(img)}')
        break
