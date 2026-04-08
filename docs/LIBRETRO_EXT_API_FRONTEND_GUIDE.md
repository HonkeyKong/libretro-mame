# libretro_ext Frontend Integration Guide

This document explains how to consume the non-standard `libretro_ext` API from a frontend.

## What this API gives you

- Driver and CPU metadata (`get_driver_name`, `get_cpu_count`, `get_cpu_tag`)
- Program counter access (`get_cpu_pc`, `get_cpu_pc_by_tag`)
- Generic CPU register/state reads by state ID (`read_cpu_state_u64_by_index`, `read_cpu_state_u64_by_tag`)
- Arbitrary memory reads/writes in CPU address spaces (`read_u8/u16/u32`, `write_u8/u16/u32`)
- ROM region reads/writes (`read_region`, `write_region`)
- Timing and frame counters
- Watch rules and hit capture
- DIP switch enumeration and updates

## Exported symbols

The core exports:

- `libretro_ext_get_api`
- `libretro_ext_get_api_v5` (compat alias)
- `libretro_ext_get_api_abi_version`
- `libretro_ext_get_api_struct_size`

Your frontend should try `libretro_ext_get_api` first, then `libretro_ext_get_api_v5`.

## ABI and safety rules

The extension is currently ABI version `5`.

At runtime:

1. Resolve get-api symbol.
2. Get `abi_version` and `sizeof_struct` from helper symbols when available.
3. Fallback to reading the first two fields from the returned struct.
4. Verify `abi_version == 5`.
5. Never call function pointers outside the returned `sizeof_struct` range.
6. Null-check each optional function pointer before calling.

## C/C++ loading pattern

```c
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef const struct libretro_ext_api* (*get_api_fn_t)(void);
typedef uint32_t (*get_u32_fn_t)(void);

static const struct libretro_ext_api* load_ext_api(void* core_handle)
{
    void* sym_get_api = dlsym(core_handle, "libretro_ext_get_api");
    void* sym_get_api_v5 = dlsym(core_handle, "libretro_ext_get_api_v5");
    void* sym_get_abi = dlsym(core_handle, "libretro_ext_get_api_abi_version");
    void* sym_get_size = dlsym(core_handle, "libretro_ext_get_api_struct_size");

    get_api_fn_t get_api = sym_get_api ? (get_api_fn_t)sym_get_api
                                       : (get_api_fn_t)sym_get_api_v5;
    if (!get_api)
        return NULL;

    const struct libretro_ext_api* api = get_api();
    if (!api)
        return NULL;

    uint32_t abi = sym_get_abi ? ((get_u32_fn_t)sym_get_abi)() : api->abi_version;
    uint32_t size = sym_get_size ? ((get_u32_fn_t)sym_get_size)() : api->sizeof_struct;

    if (abi != 5)
        return NULL;

    // Optional: require enough size for fields you intend to use.
    if (size < offsetof(struct libretro_ext_api, get_cpu_pc) + sizeof(api->get_cpu_pc))
        return NULL;

    return api;
}
```

## Common usage examples

### 1) Read PC by CPU index

```c
uint64_t pc = 0;
if (api->get_cpu_pc)
    pc = api->get_cpu_pc(0);
```

### 2) Read PC by CPU tag

```c
uint64_t pc = 0;
if (api->get_cpu_pc_by_tag)
    pc = api->get_cpu_pc_by_tag(":maincpu");
```

### 3) Read generic CPU state/register by ID

Use `read_cpu_state_u64_by_tag` or `read_cpu_state_u64_by_index`.

Known generic IDs:

- `-1` -> `STATE_GENPC`
- `-2` -> `STATE_GENPCBASE`
- `-3` -> `STATE_GENFLAGS`

```c
uint64_t value = 0;
if (api->read_cpu_state_u64_by_tag)
{
    // Example: read current PC using generic state id -1
    if (api->read_cpu_state_u64_by_tag(":maincpu", -1, &value))
    {
        // value now contains STATE_GENPC
    }
}
```

Notes:

- `state_id` values are CPU-core-specific beyond the generic negative IDs.
- Function returns `false` if CPU tag/index is invalid, state is unavailable, or output pointer is null.

### 4) Read arbitrary emulated memory

```c
uint8_t v = 0;
if (api->read_u8)
    v = api->read_u8(":maincpu", "program", 0xFF8000);
```

Spaces currently supported by name:

- `"program"`
- `"data"`
- `"io"`
- `"opcodes"`

### 5) Write arbitrary emulated memory

```c
if (api->write_u16)
    api->write_u16(":maincpu", "program", 0xFF8002, 0x1234);
```

## Suggested frontend wrapper shape

Keep one wrapper object with:

- `const libretro_ext_api* api`
- cached `abi_version`
- cached `sizeof_struct`
- helper methods that check function pointer existence

Example helper policy:

- `has_get_cpu_pc_by_tag()` checks `sizeof_struct` and pointer non-null
- `read_state_u64_by_tag(...)` returns `{ok, value}` style result

## Suggested Copilot prompts for your frontend

Use these prompts in your frontend repo:

- `Implement a C++ wrapper around libretro_ext with ABI-safe function availability checks using sizeof_struct and null function pointers.`
- `Add a method readGenPcByTag(" :maincpu ") that uses get_cpu_pc_by_tag when available and falls back to read_cpu_state_u64_by_tag(state_id=-1).`
- `Add a method readMemoryU8(tag, space, addr) with guard rails and structured error logging.`
- `Generate unit tests for extension function availability checks using fake struct sizes and null pointers.`

## Troubleshooting

- No symbol found: ensure the loaded core build actually exports `libretro_ext_get_api`.
- ABI mismatch: reject extension path and continue with standard libretro flow.
- Null function pointer: treat as optional capability not present in this build.
- `read_cpu_state_u64_*` returns false: verify CPU tag/index and state ID support for that CPU core.

## Validation tool

You can use the probe utility in this repo:

- `tests/libretro_ext_probe.c`

It verifies extension symbols, ABI version, and struct size.
