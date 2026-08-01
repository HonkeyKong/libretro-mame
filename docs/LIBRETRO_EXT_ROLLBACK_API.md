# libretro_ext Rollback API

This core exposes a separate optional rollback interface in addition to the
existing `libretro_ext_api` and the normal libretro save-state callbacks.

## Discovery

Load the core as usual, then look up:

- `libretro_ext_get_rollback_api`
- `libretro_ext_get_rollback_api_v1`
- `libretro_ext_get_rollback_api_abi_version`
- `libretro_ext_get_rollback_api_struct_size`

The returned `libretro_ext_rollback_api` is versioned and fixed-size. Frontends
should check both `abi_version` and `sizeof_struct` before calling tail fields.

## State Format

The rollback state is a fixed-size blob for the lifetime of the loaded game.
It consists of a public header followed by the compact rollback payload:

```c
struct retro_ext_rollback_state_header {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint64_t compatibility_id;
    uint64_t frame_number;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t flags;
    uint32_t reserved;
};
```

Validation rules:

- `magic`, `format_version`, and `header_size` must match.
- `compatibility_id` must match the currently loaded game/core layout.
- `payload_size` must match the loaded game's rollback payload size.
- `payload_crc32` must match the payload bytes.
- `reserved` must be zero.

The payload is the core's filtered rollback save-state data. The state header is
always written deterministically.

## Delta Format

Rollback deltas encode the XOR difference between two framed rollback states.
Blocks are fixed-size and emitted in ascending block order.

```c
struct retro_ext_rollback_delta_header {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint64_t compatibility_id;
    uint64_t from_frame;
    uint64_t to_frame;
    uint32_t state_size;
    uint32_t block_size;
    uint32_t changed_block_count;
    uint32_t encoded_size;
    uint32_t payload_crc32;
    uint32_t flags;
    uint32_t reserved;
};

struct retro_ext_rollback_delta_block {
    uint32_t block_index;
    uint16_t data_size;
    uint16_t reserved;
};
```

Validation rules:

- `magic`, `format_version`, and `header_size` must match.
- `compatibility_id` and `state_size` must match the target state.
- `block_size` must match the advertised preferred block size.
- `encoded_size` must equal the actual delta buffer size.
- `changed_block_count` must match the number of emitted blocks.
- Block indices must be strictly increasing.
- Block lengths must stay within the target state payload bounds.
- `payload_crc32` must match the encoded delta body.
- `reserved` must be zero.

Applying a delta is in-place and symmetric. Applying the same XOR delta twice
returns the original blob if the caller supplies the correct current frame.

## Reported Capability Flags

The rollback info structure reports fixed-size and XOR-delta capabilities and
whether the payload excludes immutable/static data.

## Mutable State Included

The rollback payload is intended to contain the mutable emulated machine state
needed for deterministic continuation, including:

- CPU register and execution state
- writable RAM and mutable VRAM
- timers and interrupts
- DMA and bank-switch state
- sound chip state
- mutable device registers
- input-latch state
- scheduler-visible mutable machine state
- gameplay-relevant NVRAM
- PRNG state

## Excluded Regions

The filtered rollback payload excludes data that is immutable, derived, or
reconstructible:

- CPS3 SIMM flash / game-ROM backing data
- decrypted game ROM buffers
- static memory regions tagged as rollback baselines
- immutable shared buffers tagged as startup-only data

These exclusions are safe because the omitted data can be reconstructed from the
loaded content, the game's static assets, or the machine initialization path.

## Arcadia Integration

Recommended flow:

1. Call `libretro_ext_get_rollback_api()` and verify the API version.
2. Query `get_info()` to obtain the compatibility ID, state size, and delta
   block size.
3. Allocate fixed buffers using `state_size` and `maximum_delta_size`.
4. Call `serialize()` once per frame to capture the current rollback state.
5. Compare `compatibility_id` before applying saved states or deltas from disk
   or from a peer.
6. Use `create_delta()` only when the source and destination states share the
   same compatibility ID.
7. Use `apply_delta()` only on the exact framed state it was generated for.

Do not depend on internal MAME object addresses, device pointers, or memory
tags. Use the API headers as the compatibility boundary.
