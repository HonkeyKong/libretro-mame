# Implementing the libretro_ext API in another core

This document is the core-authoring contract for `libretro_ext`. The API is a
small, optional, non-standard ABI used by cooperating frontends for inspection,
debugging, live patching, input metadata, and (optionally) rollback support. It
does not replace the normal libretro API. A core that does not implement it must
continue to work normally, and a frontend must treat its absence as expected.

The consumer-side loading procedure is documented in
[LIBRETRO_EXT_API_FRONTEND_GUIDE.md](LIBRETRO_EXT_API_FRONTEND_GUIDE.md). The
rollback interface has a separate contract in
[LIBRETRO_EXT_ROLLBACK_API.md](LIBRETRO_EXT_ROLLBACK_API.md).

## Compatibility target

The current primary interface is `libretro_ext_api` ABI version `5`.

Export these C-linkage symbols from the core:

```c
const struct libretro_ext_api *libretro_ext_get_api(void);
const struct libretro_ext_api *libretro_ext_get_api_v5(void); /* compatibility alias */
uint32_t libretro_ext_get_api_abi_version(void);
uint32_t libretro_ext_get_api_struct_size(void);
```

`libretro_ext_get_api()` and `_v5()` return the same process-lifetime API
table, or `NULL` if the extension is not available. The two numeric helpers
return `5` and the number of bytes actually exposed by that table.

The API table is append-only. Keep the original v5 fields in their exact order,
append new function pointers at the end, and leave `abi_version` at `5` when
adding backwards-compatible capabilities. A consumer may have loaded an older
table, so it must never read or call a field beyond `sizeof_struct`. A null
function pointer also means that capability is unavailable. If an incompatible
layout is ever required, publish a new ABI and new discovery symbols instead of
reordering fields.

The public ABI structs must not contain pointers to core-owned temporary data,
STL containers, emulator object pointers, or other implementation details. The
current header is C++ because it is included by the MAME implementation; the
exported table and exchange structures must nevertheless retain stable,
fixed-layout ABI-compatible representations.

## Lifetime and common failure behavior

Implement every function so it is safe to call before `retro_load_game()`,
between games, and after `retro_unload_game()`. In the no-machine state:

| Return type | Required result |
| --- | --- |
| pointer | `NULL` |
| `bool` | `false` |
| count | `0` |
| size, counter, or value | `0` |
| `void` | no effect |

Invalid indices, tags, spaces, ranges, null output pointers, and unavailable
CPU states should use the same failure result. Do not crash, assert in a
release core, or emit a fatal log for an extension request that cannot be
served. Warnings are appropriate for rejected setup such as an invalid watch
or injection rule; ordinary unsupported capability queries should be quiet.

Reset all extension-owned transient state when a game is loaded or the machine
is otherwise recreated. At minimum this includes frame/cycle observations,
watch hits, PC history, watch rules, injection rules, cached region/share/DIP
metadata, and rollback metadata. A reset must remove installed emulator hooks
before freeing the rule objects captured by those hooks.

## Required public data definitions

Use the definitions in
`src/osd/libretro/libretro-internal/libretro_ext.h` as the ABI reference. The
following values are part of the contract:

```c
enum libretro_ext_watch_access {
    LIBRETRO_EXT_WATCH_READ  = 1,
    LIBRETRO_EXT_WATCH_WRITE = 2
};

enum libretro_ext_cpu_state_id {
    LIBRETRO_EXT_STATE_GENPC      = -1,
    LIBRETRO_EXT_STATE_GENPCBASE  = -2,
    LIBRETRO_EXT_STATE_GENFLAGS   = -3
};
```

`libretro_ext_watch_hit` contains the most recent matching event. Its
`pcHistory` array contains up to 256 PCs for the matching CPU, in oldest-to-
newest order, and `historyCount` says how many entries are valid. Clear the
record explicitly; installing or clearing a rule need not manufacture a hit.
Strings copied into fixed-size fields must be NUL-terminated and truncated
safely.

## API table contract

The fields below are listed in their logical groups. They must appear in the
exact order and types in the reference header.

### Driver and CPU discovery

- `get_driver_name()` returns a stable driver/system name, or `NULL`.
- `get_cpu_count()` returns the number of addressable CPUs.
- `get_cpu_tag(index)` returns the emulator's stable tag for that CPU, or
  `NULL` for an invalid index. Tags are opaque strings; do not require
  `:maincpu`.
- `get_cpu_pc(index)` returns the current PC, or zero when unavailable.
- `get_cpu_pc_by_tag(tag)` is the tag equivalent.
- `get_cpu_total_cycles_by_tag(tag)` returns the CPU's monotonically increasing
  execution-cycle count when the emulator exposes one, otherwise zero.

CPU enumeration order must remain stable for the lifetime of a loaded game.
Tags must be accepted exactly as returned by `get_cpu_tag()`.

### CPU state and register access

Implement the index and tag forms of:

- `read_cpu_state_u64_by_index`
- `read_cpu_state_u64_by_tag`
- `write_cpu_state_u64_by_index`
- `write_cpu_state_u64_by_tag`

The three negative state IDs above are generic. Other positive IDs are
emulator/CPU-specific and should be documented by that core. Return `false`
when the CPU, state ID, or output pointer is invalid. Writes should take effect
on the live CPU state and return `false` when the state is read-only or cannot
be represented.

`read_cpu_register_by_tag` and `write_cpu_register_by_tag` are a convenience
layer for stable human-readable names. At minimum, generic `PC`, `GENPC`, and
`GENPCBASE` may map to the negative IDs. CPU-family names such as M68K `D0`–
`D7`, `A0`–`A7`, `SP`, and `SR` are optional and must return `false` when the
target CPU does not support them. Register names should be case-insensitive
only if the implementation documents that behavior; consumers should use the
canonical names.

### Address-space memory access

`read_u8/u16/u32` and `write_u8/u16/u32` operate on the selected CPU's live
address space. The reference names are `"program"`, `"data"`, `"io"`, and
`"opcodes"`; unsupported spaces return zero or perform no write. Reads and
writes must use the emulator's normal address-space accessors so device side
effects, address masks, endianness, and handlers are honored. The width is the
requested access width, not necessarily the CPU's native register width.

### Memory-region access

`get_region_count()` and `get_region_tag(index)` enumerate stable memory-region
tags. `get_region_size(tag)` returns the byte size. `read_region()` and
`write_region()` copy raw bytes from/to the region and return the number of bytes
transferred. Clamp a request at the region end; return zero for an invalid tag,
null buffer, or offset outside the region. These functions are raw region
access, not CPU bus access, so they do not promise device side effects.

Pointers or strings returned by enumeration functions must remain valid until
the next machine reset or cache rebuild, not merely until the function returns.

### Timing

- `get_frame_number()` returns a core-maintained frame counter. Increment it at
  the same point on every completed emulated frame and reset it with extension
  state.
- `get_time_attoseconds()` returns emulated time as total attoseconds. Return
  zero if no machine is active or if the value cannot be represented.
- `get_cpu_total_cycles_by_tag()` is the CPU-specific counter described above.

Do not substitute wall-clock time for emulated time. A frontend uses these
values to correlate watch hits and scripted events.

### Watchpoints and PC history

`set_debug_extensions_enabled(true)` enables recording and debug hooks;
`false` disables them and clears extension state. A core may require this to be
enabled before `retro_load_game()` so its debugger/watchpoint objects are
created during machine construction. `get_debug_extensions_enabled()` reports
the current setting.

Implement:

```c
void clear_watch_rules(void);
void add_watch_rule(const char *cpu_tag, uint64_t start, uint64_t end,
                    uint8_t access, uint8_t width);
bool get_last_watch_hit(struct libretro_ext_watch_hit *out);
void clear_last_watch_hit(void);
void check_watch_hit(const char *cpu_tag, uint64_t pc, uint64_t address,
                     uint32_t value, uint8_t access, uint8_t width,
                     uint64_t total_cycles);
```

The core must call `check_watch_hit()` (or its internal equivalent) from the
actual memory access/debug path. A rule matches CPU tag, inclusive address
range, an overlapping access mask, and width. Width `0` means any width;
otherwise the supported values are `1`, `2`, and `4` bytes. `access` uses the
read/write bits above. Record the first matching rule's event as the last hit;
include the effective value, PC, frame, cycle count, and PC history.

If the emulator has a native watchpoint facility, install and remove native
watchpoints as rules change. If it does not, call the checker from equivalent
bus hooks. Never report a watch hit solely because a frontend polled the API.

`libretro_ext_record_pc(cpu_tag, pc)` should be called from each CPU execution
path when debug extensions are enabled. Keep a 256-entry per-CPU ring. It is
diagnostic context only and is not emulation state.

### Read injection

The optional tail functions are:

```c
void add_inject_rule(const char *cpu_tag, uint64_t start, uint64_t end,
                     uint32_t value, uint8_t width, bool one_shot);
void clear_inject_rules(void);
void add_inject_rule_ex(const char *cpu_tag, uint64_t start, uint64_t end,
                        uint32_t value, uint8_t width, bool one_shot,
                        bool has_match_value, uint32_t match_value,
                        uint32_t match_mask);
```

An injection rule applies only to reads in the inclusive range on the CPU's
program/address space. Width `0` matches any access width; otherwise use `1`,
`2`, or `4` bytes. Replace the value delivered to the CPU inline in the read
path. For the extended form, inject only when:

```text
(original_bus_value & match_mask) == (match_value & match_mask)
```

When `has_match_value` is false, the rule is unconditional. A one-shot rule is
consumed only after a successful injection; a failed value match does not
consume it. Clearing rules must unregister underlying taps/hooks first.

Injection is a debug capability and must be disabled when debug extensions are
disabled. Do not implement it as a later write: that changes CPU behavior and
cannot emulate read-only or device-backed locations correctly.

### DIP switch metadata

`get_dip_count()`, `get_dip_info(index, out)`, and `set_dip_value(index, value)`
expose logical DIP fields, not raw port bytes. Each descriptor reports a name,
port tag, mask, current masked value, default masked value, and up to 64 named
settings. Zero-initialize the output before filling it and NUL-terminate all
names. Mask values on input and return `false` for invalid indices or a field
that cannot be changed at runtime. If the emulator only applies DIP changes
on restart, either reject the write or document the required restart behavior;
do not claim the live value changed when it did not.

### Optional timing trigger

`trigger_timing_capture()` may restart a core-specific timing capture window.
Return `false` when that feature is not supported or no machine is active. It
must not affect emulation when unsupported.

### Static regions and shared buffers

The static-region and shared-buffer tail fields are metadata for rollback-aware
frontends. They are not required for the basic inspection API. If implemented:

- static regions describe immutable/startup baseline data;
- shares describe named mutable buffers and their byte sizes;
- flags must accurately identify immutable, rollback-safe, exportable, and
  importable data;
- `read_share`/`write_share` use the same clamped byte-copy semantics as region
  access;
- do not expose a buffer as importable unless writing it is safe and complete.

The flag constants and descriptor layouts are defined in the reference header.
An implementation should not advertise rollback metadata that it does not
actually use to construct a deterministic state.

## Core integration sequence

1. Add a private implementation and the public ABI declarations. Keep the
   implementation independent of frontend internals except for normal
   libretro logging and environment callbacks.
2. Construct one static API table with `abi_version = 5` and
   `sizeof_struct = sizeof(table)`. Initialize unsupported tail fields to
   `NULL`.
3. Export the four discovery symbols with C linkage and default visibility.
4. Connect the lifecycle reset to game load, machine teardown, and any path
   that recreates the emulator instance.
5. Connect the frame counter to the same completed-frame boundary used by the
   core's libretro run loop.
6. Connect PC recording and watch checking to the emulator's execution and
   memory paths, not to frontend polling.
7. Run the API probe in `tests/libretro_ext_probe.c` against the built core.
8. Test calls before load, during a loaded game, after unload, with invalid
   arguments, and with an older `sizeof_struct` view.

The minimal useful implementation is driver/CPU discovery, PC access, timing,
and live memory/region access. Watchpoints, injection, DIP metadata, and
rollback are independent capabilities and should be left null rather than
returning plausible but incorrect data.

## Verification checklist

- [ ] All four discovery symbols are exported with C linkage.
- [ ] ABI version is exactly `5`; the table is append-only.
- [ ] Every function is safe without a running machine.
- [ ] Every returned string is stable for the documented lifetime and safely
      terminated.
- [ ] Invalid tags, indices, spaces, ranges, and output pointers fail safely.
- [ ] Memory bus access preserves emulator width, endianness, masking, and side
      effects.
- [ ] Region/share copies clamp to the object and report bytes transferred.
- [ ] Debug rules are gated, deduplicated if appropriate, and removed cleanly.
- [ ] One-shot injection consumes only after a successful match and inject.
- [ ] State/register writes report unsupported or read-only states as failure.
- [ ] Optional fields are null when absent and are tested through size gating.
- [ ] Load/reset/unload do not leave hooks, stale hits, or stale cached tags.
- [ ] The standard libretro API remains fully functional with the extension
      disabled or absent.

