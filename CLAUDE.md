# Gears of War (360) → native recomp via ReXGlue 0.10

Rules only. Status, findings and next steps go in FINDINGS.md.

## Layout
- Project: ~/Documents/Programming/Recomp/Gears/Gears1
- SDK: ~/Documents/Programming/ReXGlue/linux-amd64 (CLI: ~/.local/bin/rexglue)
- Game (GOD-extracted, NEVER commit or copy into repo): ~/Downloads/Gears of War/GoW/default.xex
  - Title ID 4D5307D5, XEX filetime 2006-10-14
- Manifest: gears1_recomp_manifest.toml (0.10 multi-binary format)
- Output: generated/default/ (huge; grep it, never read it wholesale)

## Rules
- Per-binary manifest overrides go under [entrypoint.functions],
  [[entrypoint.switch_tables]], etc. Top-level [functions] is SILENTLY
  IGNORED. The online wiki (April 2026) documents the older single-config
  layout; trust include/rex/codegen/{manifest.h,config.h} over the wiki.
- Run codegen with --log_level debug --log_file <fresh file>; the log
  APPENDS, so delete it between runs.
- Codegen skips work when its stamp matches; use --ignore-stamp to force a
  full run.
- No game assets, dumps, memory images or decrypted XEX in git.
  .gitignore covers generated/ and the Ghidra project (gears_recomp.*).
- Keep manifest edits minimal; back up before changes.
- Overrides of recompiled functions (`extern "C" REX_FUNC(sub_...)`) come
  only from tools/gen_hooks.py (tools/hook_table.py -> src/hooks_gen.cpp):
  one strong override per symbol. Add hooks to the table and regenerate;
  never hand-write a second override or edit hooks_gen.cpp.
- Kernel/XAM import overrides (`extern "C" REX_FUNC(__imp__Name)`) live in
  the native seam sources (e.g. src/native_input.cpp), one definition per
  import, each with a cvar that falls back to the runtime's version via
  dlsym(RTLD_NEXT, "__imp__Name"). See NATIVE_PORT.md.
- Ghidra names only at medium confidence or better, with the evidence in a
  plate comment; otherwise leave the default name.
- Record findings in FINDINGS.md, not here.
