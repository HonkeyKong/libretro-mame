# SF2HF Timing Reverse-Engineering Notes

## Goal

Get `sf2hf` / `sf2hfu` running at the correct gameplay speed by modeling CPS1 bus contention instead of relying on the old machine clock split.

## Files Touched

- `src/mame/capcom/cps1.cpp`
- `src/mame/capcom/cps1.h`
- `src/mame/capcom/cps1_v.cpp`
- `src/osd/libretro/libretro-internal/libretro_ext.cpp`
- `src/osd/libretro/libretro-internal/libretro_ext.h`

## Current Driver State

- `sf2hfu` was switched to `cps1_12MHz`.
- A retriggerable timing capture path was added through libretro ext API v5.
- CPS-A, CPS-B, and gfxram access waits are instrumented and logged.
- Gfxram writes are bucketed into:
  - `scroll1`
  - `scroll2`
  - `scroll3`
  - `obj`
  - `other`
  - `palette`
  - `unknown`

## Key Reverse-Engineering Findings

### 1. The slowdown is overwhelmingly gfxram wait related

- CPS-A / CPS-B register waits are small.
- Gameplay speed is dominated by gfxram write contention.

### 2. SF2HF alternates sprite traffic between two object banks

- Heavy traffic appeared on page 4 and page 6.
- This matched `OBJ_BASE` flipping between `0x9100` and `0x9180`.
- Classification was updated so both the active object bank and the paired alternate bank count as `obj`.
- `obj_alt` was added as a verification counter.

### 3. The old “unknown page 8” traffic was really `OTHER_BASE`

- Remaining unknown traffic matched `page_hits other` exactly.
- Classification was corrected so writes on the active `OTHER_BASE` page count as `other`.

### 4. Bucket cleanup is basically done

- In real fight windows, `unknown` is now usually zero.
- Gameplay is mainly:
  - `obj`
  - sometimes `other`
  - minor scroll traffic

## Current Timing Model

Defined in `src/mame/capcom/cps1.h`.

Current values at the end of this session:

- `SF2HF_TIMING_CPS_REG_WAIT_CYCLES = 7`
- `SF2HF_TIMING_GFXRAM_WAIT_SCROLL1_CYCLES = 20`
- `SF2HF_TIMING_GFXRAM_WAIT_SCROLL2_CYCLES = 20`
- `SF2HF_TIMING_GFXRAM_WAIT_SCROLL3_CYCLES = 20`
- `SF2HF_TIMING_GFXRAM_WAIT_OBJ_CYCLES = 112`
- `SF2HF_TIMING_GFXRAM_WAIT_OTHER_CYCLES = 40`
- `SF2HF_TIMING_GFXRAM_WAIT_PALETTE_CYCLES = 20`
- `SF2HF_TIMING_GFXRAM_WAIT_UNKNOWN_CYCLES = 29`
- `SF2HF_TIMING_GFXRAM_WAIT_OBJ_BURST_THRESHOLD = 384`
- `SF2HF_TIMING_GFXRAM_WAIT_OBJ_BURST_EXTRA_CYCLES = 16`

## Timing / Feel Checkpoints Seen During Tuning

These are approximate results from user timing checks:

- Lower `obj` waits left gameplay much too fast, around `49s`.
- `obj = 112` produced about `54.433s` and was the best result seen so far.
- `obj = 104` regressed to about `52.633s`.
- `obj = 116` regressed to about `53.383s`.
- A targeted hot-object extra wait experiment also regressed, landing around `54.05s`.
- Raising `other` from `40` to `44` also regressed, landing around `53.2s`.
- Adding a frame-local object burst surcharge at `12MHz` with `threshold=384`, `extra=12` produced about `53.95s`.
- Raising the burst extra to `16` improved to about `55.1s`, the best `12MHz` result seen so far.
- Raising the burst extra further to `20` regressed to about `53.867s`.
- A two-tier burst ramp (`320/448`, `8/8`) regressed badly to about `50.983s`.
- Switching from a frame-total burst trigger to a run-length trigger improved again: `run_threshold=96`, `run_extra=8` produced about `55.7s`, the best `12MHz` result seen so far.
- The same burst rule with the CPU forced to `10MHz` produced about `57.3s`.

## Log Interpretation Notes

### Good signs

- Fight windows with:
  - `unknown = 0`
  - `obj` dominating gfxram cycles
  - `other` near zero or modest

### Less useful windows

- Boot / title / attract / palette-heavy transitions.
- These can have large palette or residual unknown costs and are not the main gameplay target.

## Libretro Extension Notes

API v5 adds `trigger_timing_capture()`, which was used to restart a 300-frame capture window from the frontend without rebooting the machine.

## Stability Notes

- A suspected double-free was likely heap corruption rather than a literal second free.
- The user later indicated they found the issue separately.
- No explicit fix for that root cause is documented here; only the timing work above.

## Recommended Next Step

1. Rebuild with the current constants.
2. Treat plain `OBJ = 112`, `OTHER = 40` as the current best-known checkpoint.
3. Do not reintroduce the hot-object surcharge without a stronger hypothesis; it made the benchmark worse.
4. Do not spend more time on `OTHER` constant tuning; the `40 -> 44` bump made the benchmark worse.
5. The best new lever is structural rather than scalar: the frame-local object burst surcharge improved results where scalar tuning stalled.
6. Current best `12MHz` structural model:
   - `OBJ = 112`
   - `OTHER = 40`
   - run threshold `96`
   - run extra `8`
7. If more tuning is needed at `12MHz`, adjust burst parameters first:
   - run-length triggering appears better than frame-total triggering
   - the next likely sweep is around the run trigger, not the old frame-total burst constants

## Practical Resume Summary

If resuming fresh:

- Start by reading this file.
- Then inspect:
  - `src/mame/capcom/cps1.h`
  - `src/mame/capcom/cps1_v.cpp`
  - `src/mame/capcom/cps1.cpp`
- The main open problem is no longer classification.
- The main open problem is tuning the per-bucket wait constants so SF2HF gameplay lands at the correct real-time speed.
