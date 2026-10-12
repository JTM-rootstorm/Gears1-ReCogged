#!/usr/bin/env python3
"""Delta-debug a dumped Xbox shader (NVIDIA nondeterminism bug, FINDINGS
"NVIDIA BUG BISECT"): disable ALU instruction halves in the microcode,
re-translate with rexgpu-shader-tool, replace the shader in the spike repro
capture and check whether replays are still nondeterministic.

Oracle: replay twice (a third time if those match); any screenshot difference
= RANDOM (bug present), all identical = DET. A replay that fails or times out
(GPU fault) also counts as RANDOM.

Candidates are ALU vector/scalar halves with an effect (write mask or
predicate/address side effect). Fetches and exports stay untouched so the
geometry keeps reaching the screen. Disabling: vector half -> write mask 0,
opcode add; scalar half -> write mask 0, opcode retain_prev.

Usage: tools/ucode_ddmin.py <shader.bin> <module id> <workdir> [--list]
                            [--pin 18v,21s,...]
--pin: halves that always stay enabled (e.g. the path that keeps the skinned
geometry on screen; disabling it hides the bug and only costs tests).
Resumable: results are cached in <workdir>/cache.json, and the search starts
from the largest cached RANDOM disabled set that leaves the pins enabled.
"""
import hashlib, json, os, struct, subprocess, sys, tempfile

SHADER, MODULE, WORK = sys.argv[1], sys.argv[2], sys.argv[3]
CAP = os.path.expanduser('~/Documents/Programming/Recomp/Gears/gears1_captures/cutscene/spike_frame119553.gfxr')
FRAME = '119553'
SDK_BIN = os.path.expanduser('~/Documents/Programming/ReXGlue/rexglue-sdk/out/linux-amd64/Debug')
TOOL = os.path.join(SDK_BIN, 'rexgpu-shader-tool')
os.makedirs(WORK, exist_ok=True)
LOG = open(os.path.join(WORK, 'log.txt'), 'a')

def log(*a):
    msg = ' '.join(str(x) for x in a)
    print(msg, flush=True)
    LOG.write(msg + '\n'); LOG.flush()

data = bytearray(open(SHADER, 'rb').read())
be = lambda o: struct.unpack_from('>I', data, o)[0]
header_size = be(4)
record = be(0x18)
prog_off, prog_size = be(record), be(record + 4)
base = header_size + prog_off
dwords = prog_size // 4

# Control flow: pairs of 48-bit CF instructions in 3 dwords, at the start of
# the program, up to the lowest exec address. Use the translator's
# disassembly to know which instruction slots are ALU (not fetch).
disasm = subprocess.run(
    [TOOL, SHADER, os.path.join(WORK, 'orig')], capture_output=True, text=True,
    env=dict(os.environ, LD_LIBRARY_PATH=SDK_BIN))
lines = open(os.path.join(WORK, 'orig.ucode.txt')).read().split('\n')
import re
slot_kind = {}
for l in lines:
    m = re.match(r'/\*\s+(\d+)\s+\*/\s+(.*)', l)
    if m:
        slot, text = int(m.group(1)), m.group(2).strip()
        text = re.sub(r'^\((!?p0)\)\s*', '', text)
        slot_kind[slot] = 'fetch' if text.startswith(('vfetch', 'tfetch')) else 'alu'

def get_alu(slot):
    o = base + slot * 12
    return [be(o + 4 * i) for i in range(3)]

candidates = []
for slot, kind in sorted(slot_kind.items()):
    if kind != 'alu':
        continue
    w0, w1, w2 = get_alu(slot)
    export = (w0 >> 15) & 1
    if export:
        continue
    vmask, smask = (w0 >> 16) & 0xF, (w0 >> 20) & 0xF
    sop, vop = (w0 >> 26) & 0x3F, (w2 >> 24) & 0x1F
    # Scalar side effects: maxas (23), maxasf, setp* (setp ops 26..33 range),
    # kill*; anything but retain_prev (50) with mask 0 may still matter.
    if vmask or vop in (21, 22, 23, 24, 29):  # setp*_push, maxa (29)
        candidates.append((slot, 'v'))
    if smask or sop != 50:
        candidates.append((slot, 's'))

if '--list' in sys.argv:
    for c in candidates:
        print(c)
    print(len(candidates), 'candidates')
    sys.exit()

def build(disabled):
    patched = bytearray(data)
    for slot, half in disabled:
        o = base + slot * 12
        w0 = struct.unpack_from('>I', patched, o)[0]
        w2 = struct.unpack_from('>I', patched, o + 8)[0]
        if half == 'v':
            w0 &= ~(0xF << 16)
            w2 &= ~(0x1F << 24)  # vector opcode 0 = add
        else:
            w0 &= ~(0xF << 20)
            w0 = (w0 & ~(0x3F << 26)) | (50 << 26)  # retain_prev
        struct.pack_into('>I', patched, o, w0)
        struct.pack_into('>I', patched, o + 8, w2)
    return patched

cache_path = os.path.join(WORK, 'cache.json')
cache = json.load(open(cache_path)) if os.path.exists(cache_path) else {}

def key(disabled):
    return ','.join(f'{s}{h}' for s, h in sorted(disabled))

def replay(spv_dir, shot_dir):
    try:
        r = subprocess.run(
            ['gfxrecon-replay', '--log-level', 'error', '--swapchain', 'offscreen',
             '--screenshots', FRAME, '--screenshot-format', 'png', '--screenshot-dir', shot_dir,
             '--replace-shaders', spv_dir, CAP],
            capture_output=True, timeout=240)
    except subprocess.TimeoutExpired:
        return None
    png = os.path.join(shot_dir, f'screenshot_frame_{FRAME}.png')
    if r.returncode != 0 or not os.path.exists(png):
        return None
    h = hashlib.md5(open(png, 'rb').read()).hexdigest()
    os.remove(png)
    return h

def test(disabled):
    k = key(disabled)
    if k in cache:
        return cache[k]
    with tempfile.TemporaryDirectory(dir=WORK) as tmp:
        binp = os.path.join(tmp, 'shader.bin')
        open(binp, 'wb').write(build(disabled))
        out = subprocess.run([TOOL, binp, os.path.join(tmp, 'x')], capture_output=True, text=True,
                             env=dict(os.environ, LD_LIBRARY_PATH=SDK_BIN))
        spv = os.path.join(tmp, 'x.spv')
        if out.returncode != 0 or not os.path.exists(spv):
            log('  translate failed', out.stderr.strip()[:200])
            cache[k] = 'ERROR'
        else:
            rep = os.path.join(tmp, 'rep'); os.makedirs(rep)
            os.rename(spv, os.path.join(rep, f'sh{MODULE}'))
            shots = os.path.join(tmp, 'shots'); os.makedirs(shots)
            hashes = []
            for i in range(3):
                h = replay(rep, shots)
                hashes.append(h)
                if h is None or len(set(hashes)) > 1:
                    break
                if i == 1 and len(set(hashes)) == 1:
                    continue
            cache[k] = 'RANDOM' if (None in hashes or len(set(hashes)) > 1) else 'DET'
            log(f'  disabled {len(disabled)}/{len(candidates)} -> {cache[k]} {hashes}')
    json.dump(cache, open(cache_path, 'w'))
    return cache[k]

pinned = []
if '--pin' in sys.argv:
    for tok in sys.argv[sys.argv.index('--pin') + 1].split(','):
        c = (int(tok[:-1]), tok[-1])
        assert c in candidates, f'pin {tok} is not a candidate'
        pinned.append(c)

log(f'== {SHADER}: {len(candidates)} candidates, {len(pinned)} pinned')
assert test([]) == 'RANDOM', 'unmodified shader must reproduce the bug'

def parse(k):
    return [(int(t[:-1]), t[-1]) for t in k.split(',') if t]
best = []
for k, v in cache.items():
    d = parse(k)
    if v == 'RANDOM' and not any(c in pinned for c in d) and len(d) > len(best):
        best = d
log(f'starting from {len(best)} disabled (cached)')

# ddmin over the set of ENABLED, unpinned candidates: find a minimal enabled
# set that still reproduces (everything else disabled).
enabled = [c for c in candidates if c not in best and c not in pinned]
n = 2
while len(enabled) >= 2:
    chunk = (len(enabled) + n - 1) // n
    subsets = [enabled[i:i + chunk] for i in range(0, len(enabled), chunk)]
    reduced = False
    for sub in subsets:  # try keeping only the complement (disable `sub`)
        keep = [c for c in enabled if c not in sub]
        disabled = [c for c in candidates if c not in keep and c not in pinned]
        if test(disabled) == 'RANDOM':
            enabled, n, reduced = keep, max(n - 1, 2), True
            log(f'reduced to {len(enabled)} enabled')
            break
    if not reduced:
        if n >= len(enabled):
            break
        n = min(len(enabled), n * 2)
log('MINIMAL enabled set (besides pins):', enabled)
disabled = [c for c in candidates if c not in enabled and c not in pinned]
open(os.path.join(WORK, 'minimal.bin'), 'wb').write(build(disabled))
json.dump({'enabled': enabled, 'disabled': disabled}, open(os.path.join(WORK, 'minimal.json'), 'w'))
