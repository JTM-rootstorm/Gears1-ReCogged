#!/usr/bin/env python3
# Find the draw that produces a visual artifact in a GFXR capture.
# Usage: tools/gfxr_artifact_bisect.py <capture.gfxr> <frame> <artifact box> <neighbour box> <lo> <hi>
#   boxes are x0,y0,x1,y1 in full-res screenshot pixels; lo,hi = grey value range of the artifact.
# The artifact counts as GONE only when the neighbour box is still lit (mean >= hi+1), the artifact
# box no longer matches lo..hi, and so a black screen never reads as "gone".
# Draws are disabled by patching their api_call_id so gfxrecon-replay skips them.
# Pick a frame that shows in-capture rendering: in trimmed captures the first presents
# show the front buffer resolved before the trigger.
import os, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxr_blocks import walk
from PIL import Image

cap, frame = sys.argv[1], sys.argv[2]
abox = tuple(map(int, sys.argv[3].split(','))); nbox = tuple(map(int, sys.argv[4].split(',')))
lo, hi = int(sys.argv[5]), int(sys.argv[6])
data, blocks, _ = walk(cap)
draws = [(i, off) for i, off, t, api in blocks if api in (0x1106a, 0x1106b)]


def present(neuter):
    buf = bytearray(data)
    for i, off in neuter: struct.pack_into('<I', buf, off + 12, 0x0001FFF0)
    d = tempfile.mkdtemp(dir=os.path.dirname(os.path.abspath(cap)))
    tmp = os.path.join(d, 'probe.gfxr'); open(tmp, 'wb').write(buf)
    subprocess.run(['timeout', '900', 'gfxrecon-replay', '--log-level', 'error', '--swapchain', 'offscreen',
                    '--screenshots', frame, '--screenshot-format', 'png', '--screenshot-dir', d, tmp], capture_output=True)
    p = os.path.join(d, f'screenshot_frame_{frame}.png')
    im = Image.open(p).convert('L')
    for f in os.listdir(d): os.remove(os.path.join(d, f))
    os.rmdir(d)
    a = [im.getpixel((x, y)) for x in range(abox[0], abox[2], 2) for y in range(abox[1], abox[3], 2)]
    n = [im.getpixel((x, y)) for x in range(nbox[0], nbox[2], 2) for y in range(nbox[1], nbox[3], 2)]
    match = sum(lo <= v <= hi for v in a) / len(a); nmean = sum(n) / len(n)
    info = (round(match, 2), round(sum(a) / len(a), 1), round(nmean, 1))
    if nmean < hi + 1:
        return 'blind', info          # neighbourhood not lit: can't tell
    return ('gone' if match < 0.3 else 'present'), info


def search(cands, depth=0):
    """Return draws whose removal alone makes the artifact disappear (recursive split)."""
    if not cands: return []
    state, info = present(cands)
    print('  ' * depth + f'{len(cands)} draws [{cands[0][0]}..{cands[-1][0]}] -> {state} {info}', flush=True)
    if state == 'present': return []
    if len(cands) == 1: return cands if state == 'gone' else []
    mid = len(cands) // 2
    found = search(cands[:mid], depth + 1)
    if found: return found
    return search(cands[mid:], depth + 1)


print('draws', len(draws), 'baseline', present([]), flush=True)
hits = search(draws)
print('culprit draw(s):', [i for i, _ in hits])
