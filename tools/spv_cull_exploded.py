#!/usr/bin/env python3
# Instrument a Xenos-translated vertex shader (spirv-dis output): before each
# gl_Position store, replace positions that are NaN or far outside the view
# volume (|xyz| > 64*|w| + 1e4) with a culled position (w = -1).
# Usage: tools/spv_cull_exploded.py in.spvasm out.spvasm   (exit 3 if no position store)
import re, sys

lines = open(sys.argv[1]).read().split('\n')
pos = {m.group(1) for l in lines for m in [re.match(r'\s*(%\w+) = OpAccessChain %_ptr_Output_v4float %_ %int_0$', l)] if m}
if not pos or not any('%main = OpFunction' in l for l in lines):
    sys.exit(3)
decl = ['%ce_m1 = OpConstant %float -1', '%ce_z = OpConstant %float 0',
        '%ce_culled = OpConstantComposite %v4float %ce_z %ce_z %ce_z %ce_m1',
        '%ce_k = OpConstant %float 64', '%ce_b = OpConstant %float 10000',
        '%ce_v3 = OpTypeVector %float 3', '%ce_b3 = OpTypeVector %bool 3']
out, n, done = [], 0, False
for l in lines:
    if not done and l.strip().startswith('%main = OpFunction'):
        out.extend(decl); done = True
    m = re.match(r'(\s*)OpStore (%\w+) (%\w+)$', l)
    if m and m.group(2) in pos:
        n += 1; ind, p, v = m.groups()
        out += [f'{ind}%ce_xyz{n} = OpVectorShuffle %ce_v3 {v} {v} 0 1 2',
                f'{ind}%ce_w{n} = OpCompositeExtract %float {v} 3',
                f'{ind}%ce_aw{n} = OpExtInst %float %1 FAbs %ce_w{n}',
                f'{ind}%ce_lim{n} = OpExtInst %float %1 Fma %ce_aw{n} %ce_k %ce_b',
                f'{ind}%ce_lv{n} = OpCompositeConstruct %ce_v3 %ce_lim{n} %ce_lim{n} %ce_lim{n}',
                f'{ind}%ce_axyz{n} = OpExtInst %ce_v3 %1 FAbs %ce_xyz{n}',
                f'{ind}%ce_gt{n} = OpFOrdGreaterThan %ce_b3 %ce_axyz{n} %ce_lv{n}',
                f'{ind}%ce_nan{n} = OpIsNan %v4bool {v}',
                f'{ind}%ce_g{n} = OpAny %bool %ce_gt{n}',
                f'{ind}%ce_na{n} = OpAny %bool %ce_nan{n}',
                f'{ind}%ce_bad{n} = OpLogicalOr %bool %ce_g{n} %ce_na{n}',
                f'{ind}%ce_sel{n} = OpSelect %v4float %ce_bad{n} %ce_culled {v}',
                f'{ind}OpStore {p} %ce_sel{n}']
        continue
    out.append(l)
open(sys.argv[2], 'w').write('\n'.join(out))
