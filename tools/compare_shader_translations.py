#!/usr/bin/env python3
"""Check that offline translation reproduces the runtime's: for every shader
the runtime translated (dumped with --dump_shaders=<dir>: shader_<ucode
hash>.ucode.bin.<stage> microcode in host byte order and
shader_<hash>_<modification>.vk.bin.<stage> SPIR-V), translate the same
microcode with rexgpu-shader-tool using the runtime's modification and
compare the SPIR-V byte for byte.

The microcode comes from the package shader caches (tools/upk.py
--extract-shaders) when they hold the same program; otherwise (vertex
shaders patched by D3D at bind time) from the runtime's own dump, wrapped in
a minimal shader container.

Usage: tools/compare_shader_translations.py <tool> <runtime dump dir>
           <extracted shader dir> <work dir> [tool options...]
Tool options must match the runtime's device features (e.g. --rte).
"""
import collections, concurrent.futures, glob, os, re, struct, subprocess, sys

def program(container):
    header_size = struct.unpack_from('>I', container, 4)[0]
    record = struct.unpack_from('>I', container, 0x18)[0]
    offset, size = struct.unpack_from('>2I', container, record)
    return container[header_size + offset:header_size + offset + size]

def wrap(ucode_be, is_vertex):
    """Minimal container: flags, header size, microcode size; the shader
    record at 0x20 (pointed to by +0x18) gives the program's range."""
    header = struct.pack('>3I', 0x102A1100 | is_vertex, 0x28, len(ucode_be))
    header += bytes(0x18 - len(header)) + struct.pack('>I', 0x20) + bytes(4)
    header += struct.pack('>2I', 0, len(ucode_be))
    return header + ucode_be

def main():
    tool, dump_dir, extracted_dir, work_dir = sys.argv[1:5]
    tool_options = sys.argv[5:]
    os.makedirs(work_dir, exist_ok=True)
    programs = {}
    for path in glob.glob(os.path.join(extracted_dir, '*.bin')):
        programs.setdefault(program(open(path, 'rb').read()), path)

    jobs = []
    pattern = re.compile(r'shader_([0-9A-F]{16})_([0-9A-F]{16})\.vk\.bin\.(vert|frag)$')
    for path in sorted(glob.glob(os.path.join(dump_dir, '*.vk.bin.*'))):
        m = pattern.search(path)
        ucode_hash, modification, stage = m.groups()
        ucode = open(os.path.join(dump_dir, f'shader_{ucode_hash}.ucode.bin.{stage}'), 'rb').read()
        ucode_be = b''.join(ucode[i:i + 4][::-1] for i in range(0, len(ucode), 4))
        source = programs.get(ucode_be)
        if source is None:
            source = os.path.join(work_dir, f'runtime_{ucode_hash}.{stage}.bin')
            with open(source, 'wb') as f:
                f.write(wrap(ucode_be, stage == 'vert'))
        jobs.append((path, source, modification, stage, source in programs.values()))

    def run(job):
        runtime_path, source, modification, stage, from_packages = job
        prefix = os.path.join(work_dir, os.path.basename(runtime_path).replace('.vk.bin.', '.offline.'))
        r = subprocess.run([tool, source, prefix, f'--modification={modification}', *tool_options],
                           capture_output=True, text=True, timeout=120)
        origin = 'packages' if from_packages else 'runtime ucode'
        if r.returncode != 0:
            return stage, origin, 'translate failed'
        same = open(prefix + '.spv', 'rb').read() == open(runtime_path, 'rb').read()
        return stage, origin, 'identical' if same else 'different'

    with concurrent.futures.ThreadPoolExecutor(10) as pool:
        results = list(pool.map(run, jobs))
    for key, n in sorted(collections.Counter(results).items()):
        print(f'{n:6} {key[0]:4} from {key[1]:13} {key[2]}')

if __name__ == '__main__':
    main()
