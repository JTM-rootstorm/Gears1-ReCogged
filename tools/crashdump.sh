#!/usr/bin/env bash
# Boot under gdb; on abort dump guest code around ADDR and a host backtrace.
# Usage: tools/crashdump.sh <guest_addr_hex> [bytes_before] [bytes_after] [outdir]
# Guest VA maps to host 0x100000000 + addr. Dump stays outside the repo.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADDR=$(( $1 )); BEFORE=$(( ${2:-0x40} )); AFTER=$(( ${3:-0x80} ))
OUT="${4:-/tmp/gears1_dbg}"; mkdir -p "$OUT"
START=$(( 0x100000000 + ADDR - BEFORE )); END=$(( 0x100000000 + ADDR + AFTER ))
export LD_LIBRARY_PATH="$HOME/Documents/Programming/ReXGlue/linux-amd64/lib"
gdb -q -batch \
  -ex "handle SIGSEGV nostop noprint pass" -ex "handle SIGBUS nostop noprint pass" \
  -ex "handle SIGILL nostop noprint pass" -ex "handle SIGUSR1 nostop noprint pass" \
  -ex "handle SIGPIPE nostop noprint pass" \
  -ex run -ex "bt 25" \
  -ex "dump binary memory $OUT/code.bin $(printf '0x%x' $START) $(printf '0x%x' $END)" \
  --args "$ROOT/out/build/linux-amd64-debug/gears1_recomp" \
  --game_data_root "$HOME/Downloads/Gears of War/GoW" --log_file "$OUT/run.log" \
  > "$OUT/gdb.txt" 2>&1 || true
# Disassemble (big-endian PPC); print guest addresses alongside.
python3 - "$OUT/code.bin" $(( ADDR - BEFORE )) <<'PY'
import sys,subprocess
data=open(sys.argv[1],'rb').read(); base=int(sys.argv[2])
words=[data[i:i+4] for i in range(0,len(data),4)]
src=" ".join("0x%02x"%b for w in words for b in w)
out=subprocess.run(["llvm-mc","--disassemble","-triple=powerpc64-unknown-unknown","-mcpu=pwr7"],input=src,capture_output=True,text=True).stdout
ins=[l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith(".text")]
for i,w in enumerate(words):
    print("%08X  %s  %s"%(base+4*i,w.hex(),ins[i] if i<len(ins) else "?"))
PY
