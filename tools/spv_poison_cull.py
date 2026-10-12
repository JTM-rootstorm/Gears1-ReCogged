#!/usr/bin/env python3
# Instrument a Xenos-translated vertex shader (SPIR-V assembly from spirv-dis):
# every dynamically indexed float-constant read is checked for NaN / |x| > 1e6,
# and if any check fires for a vertex, its gl_Position is replaced with a
# culled position (w = -1), so the vertex can't produce visible triangles.
# Usage: tools/spv_poison_cull.py in.spvasm out.spvasm
import re, sys

src = open(sys.argv[1]).read()
lines = src.split('\n')
out = []
# constants / types we need (unique names to avoid clashes)
decl = ['%pc_false = OpConstantFalse %bool',
        '%pc_big = OpConstant %float 1000000',
        '%pc_bigv = OpConstantComposite %v4float %pc_big %pc_big %pc_big %pc_big',
        '%pc_m1 = OpConstant %float -1',
        '%pc_zero = OpConstant %float 0',
        '%pc_culled = OpConstantComposite %v4float %pc_zero %pc_zero %pc_zero %pc_m1',
        '%pc_ptr_bool = OpTypePointer Function %bool']
dyn_chain = re.compile(r'\s*(%\w+) = OpAccessChain %_ptr_Uniform_v4float %xe_uniform_float_constants %int_0 (%\d+)$')
pos_chain = re.compile(r'\s*(%\w+) = OpAccessChain %_ptr_Output_v4float %_ %int_0$')
dyn, pos = set(), set()
for l in lines:
    m = dyn_chain.match(l)
    if m: dyn.add(m.group(1))
    m = pos_chain.match(l)
    if m: pos.add(m.group(1))
decl_done = var_done = False
n = 0
for i, l in enumerate(lines):
    s = l.strip()
    # declarations go right before the first function
    if not decl_done and s.startswith('%main = OpFunction'):
        out.extend(decl); decl_done = True
    out.append(l)
    if decl_done and not var_done and re.match(r'%\w+ = OpLabel$', s):
        out.append('%pc_poison = OpVariable %pc_ptr_bool Function %pc_false'); var_done = True
        continue
    m = re.match(r'\s*(%\w+) = OpLoad %v4float (%\w+)$', l)
    if m and m.group(2) in dyn:
        n += 1; v = m.group(1)
        out += [f'%pcn{n} = OpIsNan %v4bool {v}', f'%pca{n} = OpExtInst %v4float %1 FAbs {v}',
                f'%pch{n} = OpFOrdGreaterThan %v4bool %pca{n} %pc_bigv', f'%pco{n} = OpLogicalOr %v4bool %pcn{n} %pch{n}',
                f'%pcy{n} = OpAny %bool %pco{n}', f'%pcl{n} = OpLoad %bool %pc_poison',
                f'%pcr{n} = OpLogicalOr %bool %pcl{n} %pcy{n}', f'OpStore %pc_poison %pcr{n}']
for k, l in enumerate(out):
    m = re.match(r'(\s*)OpStore (%\w+) (%\w+)$', l)
    if m and m.group(2) in pos:
        n += 1; ind, p, v = m.groups()
        out[k] = (f'{ind}%pcp{n} = OpLoad %bool %pc_poison\n{ind}%pcs{n} = OpSelect %v4float %pcp{n} %pc_culled {v}\n'
                  f'{ind}OpStore {p} %pcs{n}')
open(sys.argv[2], 'w').write('\n'.join(out))
print(f'dynamic constant loads instrumented: {len(dyn)}, position stores guarded: {len(pos)}', file=sys.stderr)
