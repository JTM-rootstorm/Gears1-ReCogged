#!/usr/bin/env python3
"""Diff the draws of one frame between two GFXR captures (gfxrecon-convert
JSON), e.g. the recomp vs Xenia Canary on the same static scene.

Each draw is summarised with its pipeline state (depth/stencil, blend, write
masks, raster), dynamic state (viewport incl. depth range, scissor, depth
bias) and attachments (formats/samples, render area). Frames are split by
vkQueuePresentKHR; draws reach a frame when their command buffer is
submitted. The two frames are aligned on (kind, count, topology) and every
aligned pair whose state differs is printed.

Usage: gfxr_draw_diff.py A.json B.json [--frame-a N] [--frame-b N] [--all]
  Frame indices default to the second-to-last frame of each capture.
"""
import argparse, difflib, json


def load(path):
    d = json.load(open(path))
    pipelines, render_passes, framebuffers, views, images = {}, {}, {}, {}, {}
    cb_cmds, frames, cur_frame = {}, [], []
    cb_state = {}

    def st(cb):
        return cb_state.setdefault(cb, {})

    for b in d:
        f = b.get('function')
        if not f:
            continue
        n, a = f['name'], f.get('args', {})
        if n == 'vkCreateImage':
            ci = a['pCreateInfo']
            images[a['pImage']] = (ci['format'], ci['samples'], ci['extent']['width'], ci['extent']['height'])
        elif n == 'vkCreateImageView':
            ci = a['pCreateInfo']
            img = images.get(ci['image'], ('?', '?', 0, 0))
            views[a['pView']] = (ci['format'], img[1], img[2], img[3])
        elif n in ('vkCreateRenderPass', 'vkCreateRenderPass2', 'vkCreateRenderPass2KHR'):
            ci = a['pCreateInfo']
            render_passes[a['pRenderPass']] = [(x['format'], x['samples'], x['loadOp'], x['storeOp']) for x in ci['pAttachments'] or []]
        elif n == 'vkCreateFramebuffer':
            ci = a['pCreateInfo']
            framebuffers[a['pFramebuffer']] = (ci['renderPass'], ci['width'], ci['height'])
        elif n == 'vkCreateGraphicsPipelines':
            for ci, h in zip(a['pCreateInfos'], a['pPipelines']):
                pipelines[h] = summarize_pipeline(ci)
        elif n == 'vkBeginCommandBuffer':
            cb_cmds[a['commandBuffer']] = []
            cb_state[a['commandBuffer']] = {}
        elif n.startswith('vkCmd'):
            cb = a.get('commandBuffer')
            s = st(cb)
            lst = cb_cmds.setdefault(cb, [])
            if n == 'vkCmdBindPipeline' and a['pipelineBindPoint'].endswith('GRAPHICS'):
                s['pipe'] = a['pipeline']
            elif n == 'vkCmdSetViewport':
                v = a['pViewports'][0]
                s['viewport'] = tuple(round(v[k], 3) for k in ('x', 'y', 'width', 'height', 'minDepth', 'maxDepth'))
            elif n == 'vkCmdSetScissor':
                r = a['pScissors'][0]
                s['scissor'] = (r['offset']['x'], r['offset']['y'], r['extent']['width'], r['extent']['height'])
            elif n == 'vkCmdSetDepthBias':
                s['depth_bias'] = (round(a['depthBiasConstantFactor'], 4), round(a['depthBiasClamp'], 4), round(a['depthBiasSlopeFactor'], 4))
            elif n == 'vkCmdSetStencilReference':
                s['stencil_ref'] = a['reference']
            elif n == 'vkCmdBeginRenderPass':
                rb = a['pRenderPassBegin']
                fb = framebuffers.get(rb['framebuffer'], (None, 0, 0))
                atts = render_passes.get(rb['renderPass'], [])
                s['attachments'] = tuple((x[0].replace('VK_FORMAT_', ''), x[1].replace('VK_SAMPLE_COUNT_', '')) for x in atts)
                s['area'] = (rb['renderArea']['extent']['width'], rb['renderArea']['extent']['height'])
            elif n in ('vkCmdBeginRendering', 'vkCmdBeginRenderingKHR'):
                ri = a['pRenderingInfo']
                atts = []
                for c in ri.get('pColorAttachments') or []:
                    v = views.get(c['imageView'], ('?', '?'))
                    atts.append((v[0].replace('VK_FORMAT_', ''), v[1].replace('VK_SAMPLE_COUNT_', '')))
                for key in ('pDepthAttachment',):
                    da = ri.get(key)
                    if da and da.get('imageView'):
                        v = views.get(da['imageView'], ('?', '?'))
                        atts.append((v[0].replace('VK_FORMAT_', ''), v[1].replace('VK_SAMPLE_COUNT_', '')))
                # Render pass attachment order is depth first in Xenia; sort for comparability.
                s['attachments'] = tuple(sorted(atts))
                s['area'] = (ri['renderArea']['extent']['width'], ri['renderArea']['extent']['height'])
            elif n in ('vkCmdDraw', 'vkCmdDrawIndexed'):
                count = a.get('indexCount', a.get('vertexCount'))
                p = pipelines.get(s.get('pipe'), {})
                draw = {
                    'kind': 'idx' if n == 'vkCmdDrawIndexed' else 'arr',
                    'count': count, 'inst': a['instanceCount'],
                    'index': b['index'],
                    'pipe': p,
                    'viewport': s.get('viewport'), 'scissor': s.get('scissor'),
                    'depth_bias': s.get('depth_bias') if p.get('depthBiasEnable') else None,
                    'stencil_ref': s.get('stencil_ref') if p.get('stencil') else None,
                    'attachments': tuple(sorted(s.get('attachments', ()))), 'area': s.get('area'),
                }
                lst.append(draw)
        elif n == 'vkQueueSubmit':
            subs = a['pSubmits'] or []
            if isinstance(subs, dict):  # single-element arrays may be unwrapped
                subs = [subs]
            for sub in subs:
                for cb in sub['pCommandBuffers'] or []:
                    cur_frame.extend(cb_cmds.get(cb, []))
        elif n == 'vkQueuePresentKHR':
            frames.append(cur_frame)
            cur_frame = []
    frames.append(cur_frame)
    return frames


def summarize_pipeline(ci):
    p = {}
    ia = ci.get('pInputAssemblyState') or {}
    p['topology'] = ia.get('topology', '').replace('VK_PRIMITIVE_TOPOLOGY_', '')
    rs = ci.get('pRasterizationState') or {}
    p['cull'] = rs.get('cullMode')
    p['frontFace'] = rs.get('frontFace', '').replace('VK_FRONT_FACE_', '')
    p['depthBiasEnable'] = rs.get('depthBiasEnable')
    p['depthClamp'] = rs.get('depthClampEnable')
    p['polygon'] = rs.get('polygonMode', '').replace('VK_POLYGON_MODE_', '')
    ms = ci.get('pMultisampleState') or {}
    p['samples'] = ms.get('rasterizationSamples', '').replace('VK_SAMPLE_COUNT_', '')
    p['a2c'] = ms.get('alphaToCoverageEnable')
    p['sampleShading'] = ms.get('sampleShadingEnable')
    ds = ci.get('pDepthStencilState')
    if ds:
        p['depthTest'] = ds['depthTestEnable']
        p['depthWrite'] = ds['depthWriteEnable']
        p['depthCompare'] = ds['depthCompareOp'].replace('VK_COMPARE_OP_', '') if ds['depthTestEnable'] else None
        p['stencil'] = ds['stencilTestEnable']
        if ds['stencilTestEnable']:
            p['stencilFront'] = tuple(str(ds['front'][k]).replace('VK_', '') for k in ('failOp', 'passOp', 'depthFailOp', 'compareOp'))
            p['stencilBack'] = tuple(str(ds['back'][k]).replace('VK_', '') for k in ('failOp', 'passOp', 'depthFailOp', 'compareOp'))
    else:
        p['depthTest'] = None
    cb = ci.get('pColorBlendState') or {}
    blends = []
    for att in cb.get('pAttachments') or []:
        if att['blendEnable']:
            blends.append(('/'.join(att[k].replace('VK_BLEND_FACTOR_', '').replace('VK_BLEND_OP_', '') for k in (
                'srcColorBlendFactor', 'dstColorBlendFactor', 'colorBlendOp',
                'srcAlphaBlendFactor', 'dstAlphaBlendFactor', 'alphaBlendOp')), att['colorWriteMask']))
        else:
            blends.append(('off', att['colorWriteMask']))
    p['blend'] = tuple(blends)
    dyn = (ci.get('pDynamicState') or {}).get('pDynamicStates') or []
    p['dynamic'] = tuple(sorted(x.replace('VK_DYNAMIC_STATE_', '') for x in dyn))
    p['stages'] = len(ci.get('pStages') or [])
    return p


def key(d):
    return (d['kind'], d['count'], d['inst'], d['pipe'].get('topology'))


def flat(d):
    out = dict(d['pipe'])
    out.pop('dynamic', None)
    for k in ('viewport', 'scissor', 'depth_bias', 'stencil_ref', 'attachments', 'area'):
        out[k] = d[k]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a'); ap.add_argument('b')
    ap.add_argument('--frame-a', type=int); ap.add_argument('--frame-b', type=int)
    ap.add_argument('--all', action='store_true', help='print matching draws too')
    ap.add_argument('--ignore', default='', help='comma-separated state keys to ignore')
    args = ap.parse_args()
    fa, fb = load(args.a), load(args.b)
    print('frames A:', [len(f) for f in fa])
    print('frames B:', [len(f) for f in fb])
    ia = args.frame_a if args.frame_a is not None else len(fa) - 2
    ib = args.frame_b if args.frame_b is not None else len(fb) - 2
    A, B = fa[ia], fb[ib]
    print(f'comparing A frame {ia} ({len(A)} draws) vs B frame {ib} ({len(B)} draws)')
    ignore = set(filter(None, args.ignore.split(',')))
    sm = difflib.SequenceMatcher(None, [key(d) for d in A], [key(d) for d in B], autojunk=False)
    print(f'alignment ratio {sm.ratio():.3f}')
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != 'equal':
            print(f'--- {tag} A[{i1}:{i2}] B[{j1}:{j2}]')
            for d in A[i1:i2][:6]:
                print('   A', key(d), d['attachments'], 'blk', d['index'])
            for d in B[j1:j2][:6]:
                print('   B', key(d), d['attachments'], 'blk', d['index'])
            continue
        for k in range(i2 - i1):
            da, db = A[i1 + k], B[j1 + k]
            xa, xb = flat(da), flat(db)
            diffs = {f: (xa.get(f), xb.get(f)) for f in set(xa) | set(xb)
                     if f not in ignore and xa.get(f) != xb.get(f)}
            if diffs or args.all:
                print(f'draw A#{i1+k} (blk {da["index"]}) / B#{j1+k} (blk {db["index"]}) {key(da)}')
                for f, (va, vb) in sorted(diffs.items()):
                    print(f'     {f}: A={va}  B={vb}')


if __name__ == '__main__':
    main()
