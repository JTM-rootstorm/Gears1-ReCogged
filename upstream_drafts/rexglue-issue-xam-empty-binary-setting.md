# rexglue/rexglue-sdk bug report — POSTED as rexglue/rexglue-sdk#502 (2026-10-11)

Form: Bug Report. Fields below map 1:1 to the template.

---

**Title**

[Bug]: XamUserWriteProfileSettings corrupts the heap when a binary setting has size 0

---

**Description**

`XamUserWriteProfileSettings` copies a `BINARY`/`CONTENT` setting like this
([xam_user.cpp#L342-L350](https://github.com/rexglue/rexglue-sdk/blob/c94f5ebdcb3c9d1a460ca48e04f9758448f8d518/src/kernel/xam/xam_user.cpp#L342-L350)):

```cpp
std::vector<uint8_t> bytes;
if (setting.data.binary.ptr) {
  bytes.resize(binary_size);
  std::memcpy(bytes.data(), binary_ptr, binary_size);
}
```

When a title writes a setting with `size == 0` and a non-null pointer,
`bytes.data()` is null and `memcpy(nullptr, p, 0)` is undefined behaviour.
glibc declares `memcpy` `__nonnull((1, 2))`, so clang may assume `data()` is
non-null afterwards. That means `resize()` must have allocated, so `size > 0`.

clang 22 at `-O3` acts on this and removes the size check that `resize()`
inlines. With size 0 the generated code does:

```
operator new(0)            ; p
*p = 0
memset(p + 1, 0, size - 1) ; size - 1 == SIZE_MAX
```

glibc's vectorized `memset` stores the head and tail vectors before its main
loop. The tail address `p + 1 + SIZE_MAX - VEC` wraps around to just before
`p`, and the loop bound wraps too, so the call returns without faulting after
zeroing roughly 256 bytes on each side of `p`. That includes `p`'s own chunk
header and its neighbours' headers. The `free` of `bytes` at the end of that
`case` then aborts.

A clang 20 build (the v0.10.0 release `librexruntime.so`) keeps the size
check, so this only shows up with newer compilers.

A related problem in the same path: `UserProfile::Setting` has virtual
methods but no virtual destructor
([user_profile.h#L88](https://github.com/rexglue/rexglue-sdk/blob/c94f5ebdcb3c9d1a460ca48e04f9758448f8d518/include/rex/system/xam/user_profile.h#L88)).
`UserProfile::AddSetting`
([user_profile.cpp#L96](https://github.com/rexglue/rexglue-sdk/blob/c94f5ebdcb3c9d1a460ca48e04f9758448f8d518/src/system/xam/user_profile.cpp#L96))
replaces an existing setting and destroys the old one through
`unique_ptr<Setting>`. Only `~Setting` runs, so the derived `std::vector` /
`std::u16string` members leak, and the object is freed with
`sizeof(Setting)` (0x20) instead of its real size (0x38 for `BinarySetting`).
Every repeated write of a title setting hits this.

**Steps to Reproduce**

1. Build the SDK on Linux with clang 22, Release (`-O3`).
2. Run a title that writes a zero-length binary title setting. Gears of War
   (4D5307D5) does this every time it saves its title settings, e.g. when
   collecting COG tags or at the end of a chapter. It writes 63E83FFF
   (1000 B), 63E83FFE (214 B) and 63E83FFD (0 B, non-null pointer) in one
   call.
3. Alternatively, check the disassembly of
   `XamUserWriteProfileSettings_entry` in `librexruntime.so`: on the
   non-null-pointer path there is no test of the size before
   `operator new` and the `memset(p + 1, 0, size - 1)` call.

**Expected Behavior**

A zero-length binary setting is stored as an empty setting (and saved as an
empty file, which already happens: 63E83FFD is written with 0 bytes before
the abort).

**Actual Behavior**

The game thread aborts inside `XamUserWriteProfileSettings`:

```
free(): invalid pointer        (plain glibc: "free(): invalid size")
...
#6  free () from libc_malloc_debug.so
#7  rex::kernel::xam::XamUserWriteProfileSettings_entry(unsigned int, unsigned int, unsigned int, rex::MappedPtr<rex::system::xam::X_USER_PROFILE_SETTING>, rex::MappedPtr<rex::system::XAM_OVERLAPPED>) () from librexruntime.so
#8  rex::ppc::HostToGuestFunction<&rex::kernel::xam::XamUserWriteProfileSettings_entry>(PPCContext&, unsigned char*) () from librexruntime.so
#9  sub_828CD758 ()   (recompiled guest caller)
```

In the core, the freed pointer `p` has size 0 (the 63E83FFD setting), and the
memory from `p - 0x100` to `p + 0x100` is all zero, including the chunk
headers, while the heap on either side is intact. The trace above is from a
run under `LD_PRELOAD=libc_malloc_debug.so MALLOC_CHECK_=3`. Without it,
plain glibc aborts at the same `free` with "free(): invalid size".

**Platform**

Linux

**Clang version**

22.1.8 (crashes); 20.1.8 (release build, does not)

**Version / Commit**

v0.10.0 (c94f5eb)

**Additional Environment Info**

Gentoo, glibc 2.43, libstdc++ from GCC 16, `-march=x86-64-v2 -O3`.

**Related**

- Relevant code:
  - `src/kernel/xam/xam_user.cpp:342`
  - `src/system/xam/user_profile.cpp:96`
  - `include/rex/system/xam/user_profile.h:88`
- Fix:
  - Construct the vector at its size and only `memcpy` when `ptr && size`.
  - Give `Setting` a `virtual ~Setting() = default;`.
  - Ready as two commits on JTM-rootstorm/rexglue-sdk branch
    `fix/xam-profile-settings`. A PR will follow once this issue exists.
