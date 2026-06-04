# Libretro EXT Reverse Engineering and Patching Playbook

This document is meant as a handoff for other Codex instances working against this core.
Use it when the task is reverse engineering, instrumentation, or live patching through the Libretro EXT API rather than by editing driver code.

## Ground Truth

Relevant implementation files in this repo:

- `src/osd/libretro/libretro-internal/libretro_ext.h`
- `src/osd/libretro/libretro-internal/libretro_ext.cpp`
- `docs/LIBRETRO_EXT_API_FRONTEND_GUIDE.md`
- `tests/libretro_ext_probe.c`

If behavior in this document ever conflicts with the code, trust those files.

## What This API Can Do

The current EXT API supports:

- CPU enumeration by index and tag
- Live PC reads by index and tag
- Generic CPU state/register reads and writes by numeric state ID
- Named register reads and writes for M68K-family CPUs
- Arbitrary memory reads and writes in named CPU spaces
- ROM region reads and writes
- Watch rules on `AS_PROGRAM` memory accesses
- Read-side inject rules on `AS_PROGRAM`
- Frame, time, and total-cycle reads

## Hard Constraints

These matter when designing a patching plan:

- Watchpoints require debug objects. Call `set_debug_extensions_enabled(true)` before `retro_load_game()` or before reloading content.
- Inject rules are installed as direct read taps on `AS_PROGRAM`. They are not generic write hooks, and they are not register-aware by themselves.
- Watch rules only filter on CPU tag, address range, access type, and width.
- Inject rules only filter on CPU tag, address range, width, and optionally the original bus value with a mask.
- There is no built-in "only inject when PC == X" or "only watch when D0 == Y" rule.
- `get_last_watch_hit()` exposes one global last-hit struct, not a queue. Clear it after consuming it.
- Named register helpers are M68K-oriented. For non-M68K CPUs, use raw state IDs.

## Required Setup

If a task needs watchpoints, PC history, or inject taps:

1. Load the EXT API from `libretro_ext_get_api` or `libretro_ext_get_api_v5`.
2. Validate `abi_version == 5`.
3. Check `sizeof_struct` before calling tail fields.
4. Call `set_debug_extensions_enabled(true)`.
5. Only then call `retro_load_game()` or reload content.

If a task only needs direct memory reads/writes or CPU state reads/writes, enabling debug extensions is still useful, but the watchpoint timing constraint is the critical one.

## Useful Constants

Generic CPU state IDs:

- `LIBRETRO_EXT_STATE_GENPC = -1`
- `LIBRETRO_EXT_STATE_GENPCBASE = -2`
- `LIBRETRO_EXT_STATE_GENFLAGS = -3`

Watch access flags:

- `LIBRETRO_EXT_WATCH_READ = 1`
- `LIBRETRO_EXT_WATCH_WRITE = 2`
- `3` means read and write

Common width values:

- `0` means any width
- `1`, `2`, `4` are the normal cases
- `8` is effectively handled on 64-bit program spaces by the current implementation

Named M68K registers accepted by `read_cpu_register_by_tag` and `write_cpu_register_by_tag`:

- `D0`-`D7`
- `A0`-`A7`
- `SP`
- `PC`
- `SR`
- `GENPC`
- `GENPCBASE`

Example snippets below assume you already did the ABI-safe availability checks for any tail fields you use, especially:

- `get_cpu_pc_by_tag`
- `read_cpu_state_u64_by_*`
- `read_cpu_register_by_tag`
- `write_cpu_state_u64_by_*`
- `write_cpu_register_by_tag`
- `add_inject_rule`
- `clear_inject_rules`
- `add_inject_rule_ex`

## Recommended Workflow

Use this sequence unless the task clearly needs something else:

1. Enumerate CPUs and confirm the target tag.
2. Enable debug extensions before content load.
3. Install the smallest watch range that proves the behavior.
4. Run until a hit appears.
5. Inspect `hit.pc`, `hit.value`, `hit.access`, `hit.width`, `hit.totalCycles`, and `hit.pcHistory`.
6. Read any live registers needed to disambiguate the hit.
7. Decide between:
   - direct memory write
   - one-shot inject rule
   - direct register write
   - direct PC rewrite
8. Clear stale watch hits and inject rules when moving to the next experiment.

## Minimal Discovery Snippet

```c
for (int i = 0; i < api->get_cpu_count(); i++)
{
    const char *tag = api->get_cpu_tag(i);
    uint64_t pc = api->get_cpu_pc(i);
    printf("cpu[%d] tag=%s pc=0x%llX\n", i, tag ? tag : "(null)",
           (unsigned long long)pc);
}
```

Do not assume `:maincpu` until you verify it. Many games do use it, but the API already gives you the exact tags.

## Watch Recipe

Install a narrow watch first:

```c
api->clear_watch_rules();
api->clear_last_watch_hit();
api->add_watch_rule(":maincpu", 0xFF8000, 0xFF8001,
                    LIBRETRO_EXT_WATCH_WRITE, 2);
```

Poll after `retro_run()`:

```c
libretro_ext_watch_hit hit;
if (api->get_last_watch_hit(&hit))
{
    printf("WATCH cpu=%s pc=0x%llX addr=0x%llX value=0x%X access=%u width=%u frame=%llu cycles=%llu\n",
           hit.cpuTag,
           (unsigned long long)hit.pc,
           (unsigned long long)hit.address,
           hit.value,
           hit.access,
           hit.width,
           (unsigned long long)hit.frame,
           (unsigned long long)hit.totalCycles);

    api->clear_last_watch_hit();
}
```

The hit also includes `pcHistory[256]` and `historyCount`. That history is useful when the exact hit PC is late and you want the short execution trail that led into it.

## Conditional Watch Handling

There is no built-in register-aware watch rule. Do it in two stages:

1. Let the watch rule catch the address.
2. Apply your extra condition in frontend code using the hit plus live register reads.

Example: only act when a write to `0xFF8000` came from `PC == 0x1234A` and `D0 == 3`.

```c
libretro_ext_watch_hit hit;
if (api->get_last_watch_hit(&hit))
{
    api->clear_last_watch_hit();

    uint64_t d0 = 0;
    bool have_d0 = false;

    if (api->read_cpu_register_by_tag)
        have_d0 = api->read_cpu_register_by_tag(":maincpu", "D0", &d0);

    if (hit.pc == 0x1234A && have_d0 && (d0 & 0xFFFFFFFFu) == 3)
    {
        // This was the specific callsite/state combination we care about.
        api->write_u16(":maincpu", "program", 0xFF8000, 0x0000);
    }
}
```

This pattern is the normal way to express "watch address X, but only when registers or PC match Y".

## Inject Recipe

Use inject rules when you want to spoof a read result instead of permanently changing memory.

Unconditional one-shot read inject:

```c
api->clear_inject_rules();
api->add_inject_rule(":maincpu", 0xFF9000, 0xFF9000, 0x0001, 2, true);
```

That means:

- CPU tag `:maincpu`
- address `0xFF9000`
- replace the read result with `0x0001`
- width `2`
- `oneShot = true`, so it disables itself after the first successful inject

This is usually safer than a permanent memory write when you only want to fake one read path.

## Value-Matched Inject

`add_inject_rule_ex` can gate on the original bus value:

```c
api->clear_inject_rules();
api->add_inject_rule_ex(":maincpu",
                        0xFF9000, 0xFF9000,
                        0x0001, 2, true,
                        true,      // hasMatchValue
                        0x0000,    // matchValue
                        0xFFFF);   // matchMask
```

This inject only fires when:

```c
(original_bus_value & 0xFFFF) == (0x0000 & 0xFFFF)
```

Use this when the same address is read in multiple states but only one state returns a distinct original value.

## PC-Conditional Inject

There is no native "inject only when PC == X" field.
The correct pattern is to arm the inject rule only when the PC condition becomes true.

Example:

```c
uint64_t pc = api->get_cpu_pc_by_tag(":maincpu");
uint64_t d0 = 0;

if (pc == 0x123456 &&
    api->read_cpu_register_by_tag &&
    api->read_cpu_register_by_tag(":maincpu", "D0", &d0) &&
    d0 == 0x42)
{
    api->clear_inject_rules();
    api->add_inject_rule(":maincpu", 0xFF9000, 0xFF9000, 0x0001, 2, true);
}
```

That gives you effective behavior equivalent to:

- "when execution reaches `0x123456`"
- "and `D0 == 0x42`"
- "spoof the next read from `0xFF9000`"

For most patching tasks, this is the cleanest way to combine execution state with a read override.

## Register-Conditional Inject After a Watch Hit

A common pattern is:

1. Watch an address or range.
2. On hit, inspect `hit.pc` and registers.
3. If the state matches, arm a one-shot inject for the next read you want to spoof.

Example:

```c
libretro_ext_watch_hit hit;
if (api->get_last_watch_hit(&hit))
{
    api->clear_last_watch_hit();

    uint64_t a0 = 0;
    if (hit.pc == 0x1012C &&
        api->read_cpu_register_by_tag &&
        api->read_cpu_register_by_tag(":maincpu", "A0", &a0) &&
        a0 == 0xFF9000)
    {
        api->clear_inject_rules();
        api->add_inject_rule(":maincpu", 0xFF9000, 0xFF9000, 0x0001, 2, true);
    }
}
```

This is usually more reliable than trying to infer everything from the raw bus value alone.

## Direct Register Surgery

When a task says "force this branch", "skip this check", or "resume from a different state", direct register writes are often simpler than inject rules.

Example on M68K:

```c
api->write_cpu_register_by_tag(":maincpu", "D0", 1);
api->write_cpu_register_by_tag(":maincpu", "A5", 0xFF8800);
api->write_cpu_register_by_tag(":maincpu", "A7", 0x00FFFFF0);
api->write_cpu_register_by_tag(":maincpu", "SR", 0x2700);
```

Use this when the logic you want to patch is easier to bypass by changing live CPU state than by changing memory traffic.

## Direct PC Rewrite

You can redirect execution by writing `PC` directly:

```c
api->write_cpu_register_by_tag(":maincpu", "PC", 0x00123456);
```

Or with the generic state API:

```c
api->write_cpu_state_u64_by_tag(":maincpu",
                                LIBRETRO_EXT_STATE_GENPC,
                                0x00123456);
```

This is appropriate when:

- you know the exact safe continuation address
- you want to skip a failing routine
- you want to land on already-existing game code instead of injecting a fabricated read

If you redirect control flow, also fix any required registers first. A PC rewrite without the expected stack or argument registers can just move the crash.

## Direct Memory Patch vs Inject Rule

Use a direct memory write when:

- you want the patched value to persist
- the target location is normal RAM
- repeated reads should all see the same modified value

Example:

```c
api->write_u16(":maincpu", "program", 0xFF9000, 0x0001);
```

Use an inject rule when:

- the original memory should stay untouched
- you only need to spoof specific reads
- you want a one-shot result
- the location may be ROM-backed or otherwise undesirable to overwrite directly

## Non-M68K CPUs

For non-M68K targets:

- still use `get_cpu_pc_by_tag`
- still use `read_cpu_state_u64_by_tag`
- still use `write_cpu_state_u64_by_tag`
- do not assume named register helpers will work

You need the target CPU's state ID enum from its device header.
Examples in this tree:

- `src/devices/cpu/z80/z80.h`
- `src/devices/cpu/m6809/m6809.h`
- `src/devices/cpu/i86/i86.h`

Generic PC access still works through `STATE_GENPC` and `STATE_GENPCBASE`.

## Agent Rules of Thumb

When another Codex instance is asked to patch a game through this API, it should default to these heuristics:

- Prefer address watches plus frontend-side PC/register filtering over broad speculative patches.
- Prefer one-shot injects for narrow proof-of-concept patches.
- Prefer direct register writes when the task is really execution-state surgery, not bus spoofing.
- Prefer direct PC rewrite only when the landing site is already known-good.
- Clear stale watch hits before each new experiment.
- Clear inject rules before arming a new one-shot unless multiple concurrent taps are intentional.
- Keep address ranges as small as possible.
- Do not hardcode `:maincpu` until CPU tags have been enumerated.

## Things Easy to Get Wrong

- Enabling debug extensions after content is already running and then expecting watchpoints to work.
- Forgetting that `get_last_watch_hit()` is one shared last-hit record.
- Treating inject rules like conditional breakpoint actions. They are read taps, not general script hooks.
- Forgetting that inject rules currently target `AS_PROGRAM`.
- Redirecting `PC` without also fixing the live registers the destination code expects.
- Assuming named register helpers are generic across CPU families.

## Practical Decision Table

- "Tell me who touched this address and from where":
  use `add_watch_rule`, then inspect `libretro_ext_watch_hit`
- "Only react when that write came from one callsite":
  watch first, then filter on `hit.pc`
- "Only react when that write came from one register state":
  watch first, then read registers and filter in frontend code
- "Spoof the next read result once":
  `add_inject_rule(..., oneShot=true)`
- "Spoof the next read only if the original value matches":
  `add_inject_rule_ex`
- "Spoof the next read only if PC/registers match":
  wait for the PC/register condition, then arm a one-shot inject
- "Skip a branch or bypass a routine":
  write registers and/or `PC`
- "Make the patch stick in RAM":
  write memory directly

## Minimal Handoff Summary

If a Codex instance needs to reverse engineer or patch a game through Libretro EXT, the default strategy should be:

1. Enable debug extensions before load.
2. Enumerate CPU tags.
3. Install the smallest relevant watch.
4. Run until hit.
5. Filter by `hit.pc` and live registers in frontend code.
6. Apply the smallest live patch:
   - one-shot inject for one read
   - register write for state surgery
   - PC write for control-flow redirection
   - memory write for persistent RAM patch

That is the intended use pattern for the current implementation.
