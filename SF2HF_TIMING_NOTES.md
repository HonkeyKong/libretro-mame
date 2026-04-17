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
- `SF2HF_TIMING_GFXRAM_WAIT_OBJ_RUN_THRESHOLD = 96`
- `SF2HF_TIMING_GFXRAM_WAIT_OBJ_RUN_EXTRA_CYCLES = 8`

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
- Lowering the run threshold to `80` regressed badly to about `51.633s`.
- Raising the run threshold to `112` also regressed, back around `53.95s`.
- Lowering the run extra to `7` at `run_threshold=96` also regressed, landing around `53.683s`.
- Raising the run extra to `9` at `run_threshold=96` also regressed, landing around `54.017s`.
- Resetting object runs on object page changes regressed badly to about `50.85s`.
- Capping the global run surcharge at `2560` cycles per frame also regressed badly to about `51.350s`.
- Replacing the linear tail with a stepped global surcharge (`interval=2`, `cycles=16`) also regressed to about `54.350s`.
- Replacing raw run-length with object block-hotness triggering landed at about `55.55s`, the closest structural variant yet to the best `55.7s` checkpoint.
- A rerun of the same hotspot `8/8` point later landed around `53.9s`, so that model currently looks noisy rather than stably better.
- Lowering hotspot block extra to `7` regressed to about `53.75s`.
- Raising hotspot block extra to `9` regressed badly to about `51.333s`.
- The same burst rule with the CPU forced to `10MHz` produced about `57.3s`.

## Active Sweep Checkpoint

- Current active experiment:
  - temporary release fallback locked
- Restored temporary gold checkpoint:
  - frame-total burst trigger at `10MHz`
  - `obj = 112`
  - `other = 40`
  - `frame_threshold = 384`
  - `frame_extra = 12`
  - `SF2HF_TIMING_LOGGING_ENABLED = false`
  - measured exactly `57.3s`
- Repeat benchmark next candidate:
  - none
- Last measured failed sweep:
  - object block-hotness trigger
  - `obj = 112`
  - `other = 40`
  - `block_run_threshold = 8`
  - `block_run_extra = 9`
  - measured about `51.333s`
- Best structural variant so far:
  - object block-hotness trigger
  - `obj = 112`
  - `other = 40`
  - `block_run_threshold = 8`
  - `block_run_extra = 8`
  - best observed run measured about `55.55s`
- Same hotspot point showed variance on rerun:
  - `block_run_threshold = 8`, `block_run_extra = 8` -> about `53.9s`
- Other hotspot flank test:
  - `block_run_threshold = 8`, `block_run_extra = 7` -> about `53.75s`
- Other failed flank tests:
  - `run_threshold = 80`, `run_extra = 8` -> about `51.633s`
  - `run_threshold = 112`, `run_extra = 8` -> about `53.95s`
  - `run_threshold = 96`, `run_extra = 7` -> about `53.683s`
  - `run_threshold = 96`, `run_extra = 9` -> about `54.017s`
  - bank-local run counting with `run_threshold = 96`, `run_extra = 8` -> about `50.85s`
  - capped global run counting with `run_threshold = 96`, `run_extra = 8`, `frame_cap = 2560` -> about `51.350s`
  - stepped global run counting with `run_threshold = 96`, `step_interval = 2`, `step_cycles = 16` -> about `54.350s`

## Run-Length Model Interpretation

- The `80` and `112` flank tests both lost to `96`, so the threshold sweet spot appears narrow if the stopwatch measurements are representative.
- The new logs suggest a deeper issue: in heavy gameplay windows, `avg_peak_run` is nearly equal to `avg_obj-frame writes`.
- Example fight windows with `run_threshold = 112`, `run_extra = 8` showed:
  - `avg_writes` around `470-516`
  - `avg_peak_run` around `463-511`
  - `avg_obj_burst_cycles` around `2840-3222`
- That means the object traffic is often one long uninterrupted run, so the run-length trigger collapses toward a frame-total object burst with an offset:
  - effective surcharge is roughly `run_extra * (obj_writes - run_threshold)` during those windows
- Practical implication:
  - sweeping `run_threshold` alone is mostly just shifting a frame-total surcharge offset
  - both `run_extra = 7` and `run_extra = 9` lost to `8`
  - this local run-model sweep appears exhausted; `96/8` is the current best checkpoint in this family

## Next Structural Hypothesis

- The bank-local run experiment was informative but wrong for timing.
- Diagnostic result from fight windows:
  - `avg_writes` around `431-490`
  - `avg_peak_run` around `159-207`
  - `avg_page_switches` around `55`
  - stopwatch regressed hard to about `50.85s`
- Interpretation:
  - object traffic really does alternate between banks often enough to break the monolithic run
  - but decoupling bank-local runs removes too much effective contention
- New likely direction:
  - keep some cross-bank coupling, but not the fully global linear surcharge
  - bank-local decoupling was too weak
  - a simple capped global tail was still too close to the old global model
  - the stepped global tail was also still too close to the old global model
  - the next model likely needs a different state variable than raw run length

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
    - `run_threshold = 80` regressed badly to about `51.633s`
    - `run_threshold = 112` also regressed to about `53.95s`
    - `run_extra = 7` at `run_threshold = 96` also regressed to about `53.683s`
    - `run_extra = 9` at `run_threshold = 96` also regressed to about `54.017s`
    - bank-local run counting regressed badly to about `50.85s`
    - capped global run counting regressed badly to about `51.350s`
    - stepped global run counting regressed to about `54.350s`
    - best known run-model point remains `run_threshold = 96`, `run_extra = 8`
    - next model should not fully decouple the two object banks and should not be another raw run-length reshaping

## Repeat Benchmark Protocol

- Do not treat a single stopwatch result as decisive anymore.
- For any candidate worth comparing, run the same benchmark protocol `3` times before drawing conclusions.
- Record:
  - candidate config
  - run 1 time
  - run 2 time
  - run 3 time
  - simple average
  - min/max spread
- Prefer comparing:
  - current baseline: global run model `run_threshold = 96`, `run_extra = 8`
  - alternate structural candidate: hotspot model `block_run_threshold = 8`, `block_run_extra = 8`
- Only collect reduced logs when needed:
  - measured time
  - `sf2hf timing burst ...`
  - `sf2hf timing obj-frame ...`
- Only escalate to full logs if:
  - a rerun is an outlier
  - a model behaves qualitatively differently
  - a new structural hypothesis is introduced

## Repeat Benchmark Status

- Baseline global run candidate `run_threshold = 96`, `run_extra = 8`:
  - run 1: `55.650s`
  - run 2: `55.600s`
  - run 3: `55.817s`
  - average: about `55.689s`
  - min/max spread: about `0.217s`
- Hotspot candidate `block_run_threshold = 8`, `block_run_extra = 8`:
  - run 1: `54.500s`
  - run 2: `54.150s`
  - run 3: `54.200s`
  - average: about `54.283s`
  - min/max spread: about `0.350s`
- Measurement note:
  - user timing method has about `+/- 0.033s` frame-cut uncertainty
- Conclusion so far:
  - the baseline global run model clearly beats the hotspot model on repeated measurement
  - the average gap is about `1.406s`, which is much larger than the frame-cut uncertainty
  - treat global run `run_threshold = 96`, `run_extra = 8` as the current stable winner

## Practical Resume Summary

If resuming fresh:

- Start by reading this file.
- Then inspect:
  - `src/mame/capcom/cps1.h`
  - `src/mame/capcom/cps1_v.cpp`
  - `src/mame/capcom/cps1.cpp`
- The main open problem is no longer classification.
- The main open problem is tuning the per-bucket wait constants so SF2HF gameplay lands at the correct real-time speed.
