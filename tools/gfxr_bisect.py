#!/usr/bin/env python3
# Usage: tools/gfxr_bisect.py <capture.gfxr> <scratch_out.gfxr>
# Finds the first draw/dispatch (in recording order) whose inclusion makes replay
# produce a new NVIDIA Xid in the kernel log. Later draws are disabled by patching
# their api_call_id to an unknown value, which gfxrecon-replay skips.
# Global bisect: keep first k draw/dispatch blocks, neuter the rest, replay, check Xid.
import struct, sys, subprocess, time
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
from gfxr_blocks import walk
src, dst = sys.argv[1], sys.argv[2]
data, blocks, _ = walk(src)
work = [(i, off, api) for i, off, t, api in blocks if api in (0x1106a, 0x1106b, 0x1106e)]
def xids(): return subprocess.run("journalctl -k --no-pager | grep -c Xid", shell=True, capture_output=True, text=True).stdout.strip()
def faults(k):  # keep work[0:k]
    buf = bytearray(data)
    for i, off, api in work[k:]: struct.pack_into('<I', buf, off + 12, 0x0001FFF0)
    open(dst, 'wb').write(buf)
    b = int(xids())
    subprocess.run(['timeout', '300', 'gfxrecon-replay', '--log-level', 'error', '--swapchain', 'offscreen', dst], capture_output=True)
    time.sleep(1.5)
    return int(xids()) > b
lo, hi = 0, len(work)  # invariant: keep lo -> no fault, keep hi -> fault
print('total', hi, flush=True)
while hi - lo > 1:
    mid = (lo + hi) // 2
    f = faults(mid)
    print('keep', mid, 'fault' if f else 'ok', flush=True)
    if f: hi = mid
    else: lo = mid
i, off, api = work[hi - 1]
print('FIRST FAULTING work item #%d block index %d api %#x' % (hi - 1, i, api))
