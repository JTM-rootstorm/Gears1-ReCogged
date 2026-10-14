#!/usr/bin/env python3
"""Translate a directory of shader containers (tools/upk.py --extract-shaders
or the shader dump) offline with rexgpu-shader-tool, validate each module
with spirv-val (Vulkan 1.2 environment, with the block layout features the
runtime's Vulkan device enables) and summarise the failures.

Usage: tools/translate_shaders.py <tool> <shader dir> <output dir> [jobs]
Writes <output dir>/<name>.spv and .ucode.txt per shader, plus
<output dir>/results.txt (one line per shader: name, status, first error).
"""
import collections, concurrent.futures, os, subprocess, sys

def translate(tool, src, out_dir):
    name = os.path.splitext(os.path.basename(src))[0]
    prefix = os.path.join(out_dir, name)
    r = subprocess.run([tool, src, prefix], capture_output=True, text=True, timeout=120)
    if r.returncode != 0 or not os.path.exists(prefix + '.spv'):
        error = (r.stderr.strip() or r.stdout.strip() or f'exit {r.returncode}').splitlines()
        return name, 'translate', error[-1] if error else ''
    v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', '--uniform-buffer-standard-layout',
                        '--scalar-block-layout', prefix + '.spv'],
                       capture_output=True, text=True, timeout=120)
    if v.returncode != 0:
        error = v.stderr.strip().splitlines()
        return name, 'validate', error[0] if error else f'exit {v.returncode}'
    return name, 'ok', ''

def main():
    tool, src_dir, out_dir = sys.argv[1:4]
    jobs = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    os.makedirs(out_dir, exist_ok=True)
    sources = sorted(os.path.join(src_dir, f) for f in os.listdir(src_dir) if f.endswith('.bin'))
    results = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for i, result in enumerate(pool.map(lambda s: translate(tool, s, out_dir), sources)):
            results.append(result)
            if (i + 1) % 1000 == 0:
                print(f'{i + 1}/{len(sources)}', flush=True)
    with open(os.path.join(out_dir, 'results.txt'), 'w') as f:
        for name, status, error in results:
            f.write(f'{name} {status} {error}\n')
    by_status = collections.Counter((name[:2], status) for name, status, _ in results)
    print('results:', dict(sorted(by_status.items())))
    # Drop spirv-val's "error: line N:" prefix so equal messages group.
    errors = collections.Counter((status, error.split(':')[-1].strip()[:110])
                                 for _, status, error in results if status != 'ok')
    for (status, error), n in errors.most_common(15):
        print(f'{n:6} {status}: {error}')

if __name__ == '__main__':
    main()
