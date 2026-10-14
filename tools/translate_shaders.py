#!/usr/bin/env python3
"""Translate a directory of shader containers (tools/upk.py --extract-shaders
or the shader dump) offline with rexgpu-shader-tool, validate each module
with spirv-val (Vulkan 1.2 environment, with the block layout features the
runtime's Vulkan device enables) and summarise the failures.

By default each shader is translated with its default modification. With
--native-variants, each gets the variants a native renderer (1x rendering,
host depth formats) needs, from the tool's analysis of the shader alone:
- vertex shaders: interpolator mask 0xFFFF (every output, zero unless the
  shader writes it), and vertex fetches read host vertex inputs located by
  their element's usage (--host-vertex-input): the vertex layout comes from
  the pipeline, so the unpatched shaders serve every vertex declaration;
- pixel shaders: their own input mask, which is always contiguous from
  interpolator 0, so input i is at location i like the vertex shader's
  output i; depth/stencil mode kNoModifiers, plus kEarlyHint when the
  shader allows implicit early Z (draws without alpha-dependent coverage).
Equivalent to the runtime's per-pair mask (VS writes & PS inputs): inputs
the vertex shader doesn't write read 0 either way.

Usage: tools/translate_shaders.py [--native-variants] <tool> <shader dir>
           <output dir> [jobs] [-- tool options...]
Writes <output dir>/<name>[_<modification>].spv and .ucode.txt, plus
<output dir>/results.txt (one line per module: name, modification, status,
first error).
"""
import collections, concurrent.futures, os, subprocess, sys

# SpirvShaderTranslator::Modification fields (spirv_translator.h).
PS_DEPTH_STENCIL_SHIFT = 46
PS_NO_MODIFIERS, PS_EARLY_HINT = 0, 1
VS_ALL_INTERPOLATORS = 0xFFFF

def native_variants(analysis):
    """Modifications for one shader from the tool's analysis line."""
    interpolators = int(analysis['interpolators'], 16)
    if analysis['kind'] == 'vs':
        return [VS_ALL_INTERPOLATORS]
    variants = [interpolators | PS_NO_MODIFIERS << PS_DEPTH_STENCIL_SHIFT]
    if analysis['early_z_allowed'] == '1':
        variants.append(interpolators | PS_EARLY_HINT << PS_DEPTH_STENCIL_SHIFT)
    return variants

def run_tool(tool, src, prefix, options, modification=None):
    args = [tool, src, prefix, *options]
    if modification is not None:
        args.append(f'--modification={modification:016x}')
    return subprocess.run(args, capture_output=True, text=True, timeout=120)

def validate(spv):
    v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', '--uniform-buffer-standard-layout',
                        '--scalar-block-layout', spv], capture_output=True, text=True, timeout=120)
    if v.returncode == 0:
        return 'ok', ''
    error = v.stderr.strip().splitlines()
    return 'validate', error[0] if error else f'exit {v.returncode}'

def translate(tool, src, out_dir, options, variants):
    name = os.path.splitext(os.path.basename(src))[0]
    prefix = os.path.join(out_dir, name)
    r = run_tool(tool, src, prefix, options)
    if r.returncode != 0 or not os.path.exists(prefix + '.spv'):
        error = (r.stderr.strip() or r.stdout.strip() or f'exit {r.returncode}').splitlines()
        return [(name, '-', 'translate', error[-1] if error else '')]
    if not variants:
        return [(name, '-', *validate(prefix + '.spv'))]
    fields = r.stdout.split()
    analysis = dict(f.split('=', 1) for f in fields[2:])
    analysis['kind'] = fields[1]
    os.remove(prefix + '.spv')
    results = []
    variant_options = options + (['--host-vertex-input'] if analysis['kind'] == 'vs' else [])
    for modification in native_variants(analysis):
        variant = f'{prefix}_{modification:016x}'
        r = run_tool(tool, src, variant, variant_options, modification)
        if r.returncode != 0:
            results.append((name, f'{modification:016x}', 'translate', r.stderr.strip()[-200:]))
        else:
            results.append((name, f'{modification:016x}', *validate(variant + '.spv')))
    return results

def main():
    args = sys.argv[1:]
    options = args[args.index('--') + 1:] if '--' in args else []
    args = args[:args.index('--')] if '--' in args else args
    variants = '--native-variants' in args
    args = [a for a in args if a != '--native-variants']
    tool, src_dir, out_dir = args[:3]
    jobs = int(args[3]) if len(args) > 3 else 10
    os.makedirs(out_dir, exist_ok=True)
    sources = sorted(os.path.join(src_dir, f) for f in os.listdir(src_dir) if f.endswith('.bin'))
    results = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for i, shader_results in enumerate(
                pool.map(lambda s: translate(tool, s, out_dir, options, variants), sources)):
            results += shader_results
            if (i + 1) % 1000 == 0:
                print(f'{i + 1}/{len(sources)}', flush=True)
    with open(os.path.join(out_dir, 'results.txt'), 'w') as f:
        for name, modification, status, error in results:
            f.write(f'{name} {modification} {status} {error}\n')
    by_status = collections.Counter((name[:2], status) for name, _, status, _ in results)
    print(f'{len(sources)} shaders, {len(results)} modules:', dict(sorted(by_status.items())))
    # Drop spirv-val's "error: line N:" prefix so equal messages group.
    errors = collections.Counter((status, error.split(':')[-1].strip()[:110])
                                 for _, _, status, error in results if status != 'ok')
    for (status, error), n in errors.most_common(15):
        print(f'{n:6} {status}: {error}')

if __name__ == '__main__':
    main()
