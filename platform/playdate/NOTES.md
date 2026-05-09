# PokeMini Playdate Port — Development Notes

Living document of bugs found, fixes applied, and gotchas learned while porting
the PokeMini emulator to the Playdate. Update this whenever something
non-obvious is discovered. Newest entries on top.

## Build commands

The only artifact that ships is **`PokeMini.pdx`**. Both build branches
(device and sim) produce a `.pdx` with that exact name — they're meant
to be alternatives, not coexist. The device build is the canonical one;
the sim build exists only as a desktop dev convenience.

### Device build (the one that ships)

```
rm -rf build-device PokeMini.pdx
mkdir build-device && cd build-device && \
    cmake .. -DTOOLCHAIN=armgcc \
        -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake && \
    make && cd ..
```

Output: `PokeMini.pdx/pdex.bin` (the ARM device binary, packed by `pdc`).
Sideload that `.pdx` to the Playdate.

`PLAYDATE_SDK_PATH` must be **exported** in the shell — the
`-DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/...` shell-expansion fails
silently if it's unset (this user's shell rc doesn't export it; the sim
build can fall back via `~/.Playdate/config`, but the device build can't).

### Sim build (optional, desktop dev convenience)

```
rm -rf build PokeMini.pdx
mkdir build && cd build && cmake .. && make && cd ..
```

Output: `PokeMini.pdx/pdex.dylib`. Open the `.pdx` in the Playdate
Simulator app on macOS.

### Common gotchas

- **Don't run both builds against the same `Source/`.** `pdc` bundles
  whatever's in `Source/` at the moment it runs (`pdex.elf` for device,
  `pdex.dylib` for sim). If you've previously built the other target,
  the leftover binary will be packed alongside the current one. The
  resulting `.pdx` is technically still valid, but the file gets
  bigger for no reason and stale-binary debugging gets confusing —
  see the "stale `Source/pdex.elf`" gotcha in the active-gotchas list
  below.
- **`build-device/`, not `build/`, is the device build dir.** Cleaning
  only `build/` doesn't clean device output.

## The "device-only" bug pattern

Several bugs in this port have presented as **works on simulator, broken on
device**. The simulator runs the game as a macOS dylib with looser timing and
different stdlib behavior than the bare-metal ARM build. Anything below this
line is a real bug that the simulator failed to catch.

---

## Bugs fixed (in order discovered)

### 1. `_BIG_ENDIAN` macro collision (fixed)
**Symptom**: device hangs at white/black screen, simulator works.
CPU trace shows `MinxCPU.SP.D = 0x20000000` when `SP.W.L` was set to `0x2000`.

**Root cause**: `source/MinxCPU.h`, `source/MinxCPU_noBranch.h`,
`source/MinxTimers.h`, `source/Endianess.h` used `#ifdef _BIG_ENDIAN` to gate
big-endian struct layouts. The Playdate device build (newlib via
`arm-none-eabi-gcc`) defines `_BIG_ENDIAN` as the numeric constant `4321` in
`<sys/endian.h>` — pulled in transitively by `<stdio.h>`/`<stdint.h>`. macOS
libc happens not to define it.

The "BIG layout" union puts `W.L` at byte offset 2 of the 32-bit register, so
on a little-endian CPU `SP.W.L = 0x2000` ends up storing into bits 16–31 of
`SP.D`, giving `SP.D = 0x20000000`. PUSH then writes to address `0x20000000+`,
which in PERFORMANCE mode is a no-op. POP reads fresh 0xFFs and the CPU jumps
to V=0xFF land.

**Fix**: replace `#ifdef _BIG_ENDIAN` with
`#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__` in all
four headers. `_BIG_ENDIAN` is a *value* used in `__BYTE_ORDER == _BIG_ENDIAN`
comparisons, never an existence flag.

### 2. `CommandLine.synccycles` never initialized (fixed)
**Symptom**: black screen, no game running. Diagnostic shows `PC` frozen at
0x0040, `PRC=0`, `Disp=0`, `on=0`.

**Root cause**: `CommandLineInit()` in `source/CommandLine.c` zeroes the
`TCommandLine` struct via `memset` and never sets `synccycles`. With
`synccycles = 0`, the inner loop in `PokeMini_EmulateFrame()`:

```c
while (PokeHWCycles < synccylc) {  // 0 < 0 = false
    PokeHWCycles += MinxCPU_Exec();
}
```

never executes. Zero CPU instructions run per frame, the PRC counter never
reaches its 72Hz threshold, and the display stays at its initial state.

**Fix**: set `CommandLine.synccycles = 16;` in `eventHandler` after
`CommandLineInit()`. Documented default is 8; UI clamps to 8–64. 16 is a good
balance of accuracy vs. overhead — every CPU instruction triggers a full PRC
sync at 8, but at 16 we typically batch ~2 instructions per sync.

### 3. `init_expand_lut()` defined but never called (fixed)
**Symptom**: solid black screen even though the diagnostic log shows
the emulator is working: `PC` advancing into game ROM, `Disp=1`, `PRC=2`,
`on=979` (979 pixels lit in `LCDPixelsD`).

**Root cause**: a previous optimization pass added a 256-entry lookup table
(`expand_lut[256][3]`) that maps each 8-pixel PM byte to 3 expanded Playdate
framebuffer bytes (3× horizontal scale). The init function `init_expand_lut()`
is correctly written but never called from `eventHandler`. Since the table
sits in BSS, every entry is `{0, 0, 0}` — so every framebuffer byte we write
is 0, regardless of the source pixels. 0 in the Playdate 1-bit framebuffer
means "black".

**Fix**: call `init_expand_lut()` once in `kEventInit` before the first
`render_screen()`.

### 5. ROMs in `/Shared/` invisible because `kFileRead` only searches the bundle (fixed)
**Symptom**: ROMs visibly present at `/Shared/Emulation/pm/games/` on the
device (verified via USB), but the app reports "no ROMs found" and falls
through to the FreeBIOS no-cart splash. Worked fine in the simulator
when `.min` files were dropped into the app's Data folder.

**Root cause**: `pd->file->open(path, kFileRead)` searches **only the
read-only pdx bundle**, not the data side of the filesystem. Per the
SDK file API:

| flag                      | searches                                  |
| ---                       | ---                                       |
| `kFileRead`               | game pdx (read-only bundle)               |
| `kFileReadData`           | game data folder                          |
| `kFileRead\|kFileReadData`| data folder first, falls back to bundle   |
| `kFileWrite` / `kFileAppend` | always writes to data folder           |

The cross-app `/Shared/` folder (added in Playdate firmware/SDK 2.4,
mkdir/stat fixed in 2.4.1) is reached via the data side. With
`kFileRead` alone, every ROM in `/Shared/...` silently fails to open —
no error, just `NULL` returned. The app then loaded FreeBIOS only and
displayed its built-in "no cart" screen, which is the symptom the user
saw.

**Fix**: open ROMs with `kFileRead | kFileReadData`. Both bundle and
`/Shared/` work; the bundle search is harmless overhead. Same flag is
the right default for any "open this file by user-supplied path" case
in this codebase.

**Diagnostic worth keeping in mind**: `pd->file->geterr()` returns the
last file-API error string. `pd->file->stat(path, &st)` is a cheap way
to check whether a directory exists before listing it — useful for
distinguishing "path didn't route correctly" from "directory is empty."

### 4. `render_screen` early-return on `!LCDDirty` was a red herring
**Symptom thought to be**: black screen because `LCDDirty` is 0.

**Reality**: the early-return is a valid optimization. `LCDDirty` is set to
non-zero by `MinxLCD_LCDWritefb` (called from `MinxPRC_CopyToLCD` whenever the
PRC fires its copy trigger), and `MinxLCD_Render` does NOT clear it. So
`LCDDirty` is non-zero on every frame the PRC produces output, and our
`render_screen` correctly clears it after the blit. The Playdate has a single
1-bit framebuffer (no double-buffering visible to user code), so skipping
rendering when nothing changed leaves the previous content displayed —
which is what we want.

The real bug was #3 (LUT not initialized). Once that's fixed, the
early-return is safe and worth keeping for performance.

---

## Active gotchas / things to remember

- **The device build is in `build-device/`, not `build/`.** Cleaning only
  `build/` leaves stale device output in `Source/pdex.elf` that can
  mask new fixes.
- **Stale `Source/pdex.elf` gotcha.** `pdc` packs whatever it finds in
  `Source/` into the `.pdx` — it does NOT trigger a rebuild of the
  binaries. So if you've ever run the sim build (or just have a leftover
  `pdex.elf` from a previous device build), `pdc` will happily bundle
  that **stale** `pdex.elf` into the new `.pdx`. The simulator branch
  picks up new C edits via its own `pdex.dylib`; the device runs the old
  code. The "works on sim, broken on device" symptom looks like a real
  device bug but has nothing to do with the device target. Either:
  (a) clean `Source/pdex.elf` and `build-device/` together before each
  device build, or (b) check `Source/pdex.elf`'s mtime against
  `PokeMini_Playdate.c`'s before sideloading.
- **Soft reset vs hard reset**: `PokeMini_Reset(0)` does a soft reset.
  This does NOT clear PM_RAM (only `PokeMini_Create` and hard reset do).
  After `PokeMini_Create`, RAM is `0xFF` everywhere. The freebios `sreset`
  path expects this and clears RAM itself via `clearram_2` before jumping
  to the game. If `sreset` doesn't run all the way through (e.g. CPU not
  executing because of bug #2), the stack is full of `0xFF` and any RET
  pops garbage into V:PC.
- **Audio runs on its own thread**. Hearing varying audio while the screen
  is wrong means the CPU emulation is correct but `render_screen` /
  framebuffer write is broken. (This is how bug #3 was localized.)
- **`pd->graphics->getFrame()` returns a pointer into a single 1-bit
  framebuffer.** `markUpdatedRows(start, end)` tells the system which rows
  to push to the panel.
- **Diagnostic `logToConsole` is expensive** — even 60 calls during startup
  noticeably delays first frame. Strip diag logs once a fix is verified.

## Performance: what landed and what was tried (2026-05-03)

Final state: **100% real PM speed for typical gameplay**, 93% under heavy
PRC load (busy sprite scenes), display steady at 30 fps. No audio/visual/
input glitches. Got there through a sequence of measure→change→measure
iterations, summarized below so future tuning starts from data, not guesses.

### What worked, in the order applied

1. **`synccycles 16 → 64`** (`PokeMini_Playdate.c:263`). The original 16 was
   carried over from the synccycles-init-bug fix and was below the
   `PERFORMANCE` default of 64. Fastpath: every sync exits the CPU inner
   loop and calls `MinxTimers_Sync()` + `MinxPRC_Sync()` (Hardware.c:55–57).
   At 16 vs 64 that's ~4× more sync overhead per frame.
   Result: gameplay went from "well under speed" to "close to real, ~83%."
2. **`-O3 -flto` for the device target** (CMakeLists.txt: added to the
   `armgcc` branch). The SDK's `arm.cmake` defaults to `-O2` with no LTO.
   `-O3 -flto` enables cross-TU inlining of `MinxCPU_Exec` → `Fetch8`
   → `MinxCPU_OnRead` (which lives in Hardware.c) and the per-cycle
   `MinxTimers_Sync` / `MinxPRC_Sync` calls, plus aggressive opcode
   handler inlining. Big win — the most impactful single change.
   Result: gameplay jumped to "very close to real, ~90%, with FPS dipping
   to 18-23 in heavy scenes."
3. **Branchless + fused `render_screen`** (`PokeMini_Playdate.c:184–222`).
   Replaced 8× conditional `if (src[i] == 0) byte |= 0x..` with a single
   expression `((src[0] == 0) << 7) | ...`, and merged the pack and expand
   passes (no `pm_row[12]` temp, write directly to all 3 destination rows
   per byte). Smaller than expected impact, see "what we measured" below.
   Result: "very close to native, slightly slower."
4. **Fractional pacing accumulator** (`PokeMini_Playdate.c:228–245`). To
   match PM's 72 Hz against Playdate's 30 Hz refresh, emulate 2 or 3 PM
   frames per Playdate frame in pattern 2,2,3,2,3 (12 PM frames per 5
   Playdate frames = 72 fps exactly). Integer accumulator: `frame_accum +=
   12; pm_frames = frame_accum / 5; frame_accum -= pm_frames * 5`.
   Result: when display can hit 30, emulation is exactly 1:1 with real PM.
5. **`synccycles 64 → 256 → 512`** (the documented max under
   `PERFORMANCE`, see UI.c:915). Cuts sync overhead another 4–8× from
   step 1. Profiling showed ~150ms/sec saved in heavy scenes from 64→256,
   another ~50ms/sec from 256→512.
   Result: light scenes hit 100% real speed with 7ms idle per call;
   heavy scenes 93%. No glitches at 512 (verified on device).

   **Do not go past 512.** Tried 2048 first (white screen + audio), then
   1024 expecting the half-step to be safe — also white screen, even
   though it ran ~50% of full speed (so the CPU loop is fine, only
   the PRC frame trigger is broken). The failure mode is the same in
   both: `LCDPixelsA` never populates, PM screen renders all-white
   over the black border, audio plays normally because it runs on a
   separate thread reading emulator state directly. 512 really is the
   ceiling on this hardware. Don't bother bisecting — even if some
   value between 513 and 1023 happened to work, the gain over 512
   would be tiny and the breakage risk varies by ROM.

### What we measured (for future reference)

After step 4, added phase timing in `update()` (since stripped). Logged
per-second totals to console. Two regimes were visible:
- **Light scenes (steady):** 30 calls/sec × 2.4 PM frames = 72 emulated/sec.
  emu=~780ms, render=~13ms, input=<1ms, per_call=~26ms (i.e. 7ms idle).
  Display capped at 30 by `setRefreshRate(30)`.
- **Heavy scenes:** 28 calls/sec × ~67 PM = 67 emulated/sec (93% real).
  emu=~970ms (saturated at 100% CPU), render=~13ms.

Per-PM-frame cost was ~11ms in light scenes, ~14.5ms in heavy — so heavy
scenes cost 30% more emulator time per PM frame. That's not opcode
dispatch overhead (which is constant per opcode) but extra per-frame work:
PRC sprite/BG draw, more game logic. To break this further you'd need to
profile inside `MinxPRC_Sync` and `MinxCPU_Exec`, not the wrapper.

The **render path was never the bottleneck** despite our suspicions: it
costs ~10–14ms per *second* (1% of CPU). Branchless+fused was a small win
but largely unmeasurable against the noise. If revisiting, leave it alone
and target the core.

### What was tried and didn't help (or actively hurt)

- **Time-based pacing** (emulate `delta_ms × 72 / 1000` PM frames, with a
  100ms cap to avoid spirals). Conceptually right — keeps emulated time
  matched to wall time across FPS dips. In practice on a saturated device,
  delta climbs to the cap, asks for 7+ PM frames per call, each call takes
  80ms+, display drops to 10–15 fps. The cap turns "slightly slow" into
  "slow AND unresponsive." Reverted in favor of the fixed 12/5 pattern.
- **`setRefreshRate(36)`** to get a clean 1:2 ratio with PM's 72 Hz. Math
  works but device couldn't sustain 36 Hz with the emulation load even
  after all wins above; would have made the floor worse, not better.

### What's NOT available on the Playdate

Investigated and ruled out — don't waste time re-investigating:
- ~~**ITCM/DTCM placement.**~~ **CORRECTION (2026-05-09):** earlier
  notes claimed `__attribute__((section(".itcm")))` wouldn't be honored
  by the Playdate loader. That was wrong — the [Playdate "dirty
  optimization secrets"
  thread](https://devforum.play.date/t/dirty-optimization-secrets-c-for-playdate/23011)
  walks through making it work. We have not implemented full ITCM
  copy-at-runtime yet, but the related "section consolidation" pass
  *was* implemented and is a real win — see the
  "Dispatcher locality pass" section below. Full ITCM remains future
  work for closing the last ~19% on heavy ROMs.
- **Native 1-bpp Video renderer** (in the `Video_x*.c` sense). The
  PokéMini renderer architecture reads from `LCDPixelsD` (1 byte per
  pixel, written by MinxLCD/MinxPRC) and produces output in some format.
  A "1-bpp variant" would still read 1 byte/pixel and pack — exactly what
  `render_screen` already does. Truly skipping `LCDPixelsD` would mean
  rewriting MinxLCD's per-pixel writes to RMW-pack bits, which is
  invasive and probably slower on Cortex-M7 (RMW vs byte store).

### Where the remaining 7% deficit lived (commercial English ROMs)

Heavy-scene saturation on the original baseline was 100% emulator-core-
bound. The "Dispatcher locality pass" below pushed light scenes to
100% real PM and brought the typical heavy-scene number from 93% to
near full speed too. The remaining unaddressed perf gap is on **heavy
Japanese ROMs** (Togepi JP), discussed at the end of that section.

## Dispatcher locality pass (2026-05-09)

### Trigger

User reported Japanese Togepi running noticeably slow vs. the
prior-baseline English Togepi. Per-second phase timing showed the
core was saturated at ~990 ms/sec emu but only emulating 41 PM
frames/sec (real PM = 72), at ~17 fps display. Inner-loop split
(temporary `PD_PERF_DIAG` instrumentation in `Hardware.c` and
`update()`) attributed the cost as:

| Phase | Heavy-scene cost |
| --- | --- |
| `cpu` (`MinxCPU_Exec` inner loop) | 62% of `emu` |
| `prc` (`MinxPRC_Sync`) | 18% |
| `timers` (`MinxTimers_Sync`) | 13% |
| `misc` (loop overhead + instrumentation) | 7% |
| `audio` (`MinxAudio_Sync`) | 0% (POKEMINI_GENSOUND path) |

So the dispatcher dominated. The 256-case `MinxCPU_Exec` switch +
its handler tables (`MinxCPU_XX/CE/CF/SP.c`) total ~19 KB of code,
which can't fit in the M7 Rev A's 4 KB I-cache. Every opcode
dispatch was likely an I-cache miss, falling back to slow SRAM.

### What worked

**Section consolidation** — tag the dispatcher and its tightest
helpers with `__attribute__((section(".text.hot")))` and place
`.text.hot` first in `.text` via a custom `link_map.ld`. The hot
block is still bigger than 4 KB, but adjacent functions share cache
lines, the start of `.text` enjoys the most-favorable alignment,
and frequently-called callees (e.g. `MinxCPU_OnRead` from inside
`MinxCPU_Exec`) are within tens of KB rather than scattered across
the binary.

Mechanism (don't skip any of these — each was load-bearing):
1. `POKEMINI_HOT` macro in `PokeMini.h`, gated on `TARGET_PLAYDATE`,
   expanding to `__attribute__((section(".text.hot")))`. On other
   platforms it expands to nothing.
2. Tag the hot functions in upstream code (yes, this touches `source/`
   — minimal one-token patches): `MinxCPU_Exec`, `MinxCPU_ExecCE`,
   `MinxCPU_ExecCF`, `MinxCPU_ExecSPCE`, `MinxCPU_ExecSPCF`,
   `MinxCPU_OnRead`, `MinxCPU_OnWrite`, `MinxTimers_Sync`,
   `MinxPRC_Sync`. Files affected: `MinxCPU_XX.c`, `MinxCPU_CE.c`,
   `MinxCPU_CF.c`, `MinxCPU_SP.c`, `Hardware.c`, `MinxTimers.c`,
   `MinxPRC.c`. Each gains a `#include "PokeMini.h"` if not already
   present.
3. Custom `platform/playdate/link_map.ld` (verbatim copy of SDK's
   except for `*(.text.hot*)` placed first inside `.text`).
4. CMakeLists must **strip the SDK's `-T` flag from `LINK_OPTIONS`
   and replace it with ours.** Without this step, ld concatenates
   the two linker scripts and the SDK's `.text` SECTIONS block wins
   (first one defines the section), so our changes do nothing. Use
   `get_target_property` + `list(FILTER ... EXCLUDE REGEX ...)`.
5. **Drop `-flto`.** This was the ugly one. With `-flto`, GCC merges
   all functions into a single `.text` at link time before the
   linker script runs, so per-function section attributes are
   ignored. Without LTO each `.c.obj` retains the section attribute.

### Result

Heavy Japanese Togepi:

| Metric | Before (`-O3 -flto`, no consolidation) | After (`-Os`, no LTO, with consolidation) |
| --- | --- | --- |
| Display fps | ~17 | ~24 |
| PM frames/sec | 41 | 58 |
| ms per PM frame | 24.1 | 17.1 |
| % real PM speed | 58% | **81%** |

A 30% reduction in per-PM-frame cost; some windows hit ~93% real PM.
Light scenes and English ROMs were already at 100% — they stay there.

### Why this beat the LTO loss

NOTES.md previously called LTO "the most impactful single change,"
predicting ~10% loss if dropped. We dropped it and *gained* 30%
overall. Two reasons:
- Section consolidation alone is worth ~40% on this workload (huge).
- LTO at `-Os` does much less than LTO at `-O3` did (less inlining
  happens at `-Os` regardless of LTO), so the LTO loss was small.

If we ever re-enable `-flto`, we'd lose section consolidation and
the net would go *backwards*. They're not stackable with the current
compile order.

### Where the remaining ~19% deficit on heavy Japanese ROMs lives

Still core-bound. The next lever is **full ITCM placement** of
`MinxCPU_Exec` (the dispatcher fits in 16 KB of ITCM, which the M7
serves at 1-cycle latency). The forum post linked above walks
through it. Outline:
- Custom `link_map.ld` adds an `__itcm_start` / `__itcm_end` region.
- `_itcm` macro: `__attribute__((section(".itcm"))) __attribute__((short_call))`.
- Tag the dispatcher and its tightest callees with `_itcm`.
- At app init, `memcpy` the ITCM section into the M7's actual ITCM
  region, fix the Thumb bit (destination must be congruent mod 2 to
  source), call `icache_flush()`.
- Mark non-ITCM callees that ITCM code calls with `__attribute__((longcall))`.

Risks: easy to crash on first attempts, requires `nm` inspection
between iterations to verify the dispatcher fits in ITCM. About a
day of focused work. Plausible result: another 30-50% reduction in
the `cpu` phase, potentially closing the gap to real-PM speed on
even the heaviest ROMs.

Not pursued yet — current 81% real-PM speed on the worst-case ROM
is shippable. Listed here so future-me knows where to look.

### Related: `-falign-loops=32`

Added alongside `-Os` as a forum-suggested cache-friendly default.
We can't isolate its individual contribution from the section
consolidation result, but it's free, safe, and theoretically right
for the M7's 32-byte cache lines, so it stays.

### Diagnostic infrastructure (kept around)

Both `Hardware.c` and `PokeMini_Playdate.c`'s `update()` had
per-second phase-timing instrumentation behind `#ifdef PD_PERF_DIAG`
during this work. The `Hardware.c` instrumentation has been stripped.
The `update()` block had to STAY — see the next entry.

### Perf-keepalive anomaly (2026-05-09, after dispatcher pass)

After confirming the section-consolidation perf win on Japanese
Togepi (24 fps, 81% real PM), we tried to ship a clean version
without the per-second phase-timing block. Result: **on-device
throughput collapsed from 24 fps to ~12 fps** even though the elf
was byte-identical in the hot path (same `nm` addresses for
`MinxCPU_Exec` and friends, same `.text` size, same compile flags).

Then we tried to find the minimum subset of the timing block that
restored perf. Empirical grid:

| Per-update content | Result |
| --- | --- |
| Nothing (clean code) | ~12 fps |
| 1× `getElapsedTime` (discarded) | ~12 fps |
| 4× `getElapsedTime` (discarded) | ~12 fps, slightly worse |
| `logToConsole("alive")` once/sec, no timing | ~12 fps |
| 4× `getElapsedTime` + math + statics, **no log** | between, ~15 fps |
| Full block (4× syscalls + math + statics + once/sec log) | **24 fps** |

Only the *full* block restores 24 fps. Neither the syscalls, the
log, nor the bookkeeping alone is sufficient. The combination is
what works.

We don't fully understand the mechanism. Best guesses, in order of
plausibility:
- **Firmware-level scheduling/clock-state** that drops to a low
  setting for purely-userland frames and is held high by some
  system-call density combined with periodic I/O activity. Any one
  signal isn't enough; the firmware seems to require both syscall
  density + periodic console/USB writes.
- **FPU pipeline warmup** plus periodic state-flushing — the
  `getElapsedTime` calls return floats and force FPU register
  loads; the `logToConsole` formatting (with float `%.1f`/`%.0f`
  args) does meaningful FP work inside the firmware. Together they
  may keep the FPU from a cold-start latency we'd otherwise pay
  inside the emulator's float-using code (`render_screen`'s
  threshold computation, `handle_input`'s accelerometer magnitude).
- **Some unknown firmware quirk** in SDK 3.0.6 that may not exist
  in earlier or later SDK versions. Worth retesting whenever the
  SDK is updated.

### What ships

The "PERF KEEPALIVE" block in [PokeMini_Playdate.c]'s `update()`.
It is structurally a copy of the diag-on phase-timing block,
labeled to make clear that **it is not optional**: removing it
costs ~50% device throughput. Variable names use the
`keepalive_` prefix instead of `diag_` to discourage future
maintainers from "cleaning up the diagnostic code."

The block also doubles as a useful console heartbeat: it prints
one `perf:` line per second with the actual achieved fps and PM
frames/sec, which is helpful for users reporting performance
issues. So even though the *reason* it exists is performance
rather than diagnostics, the side effect is valuable.

### When to revisit

- A future Playdate SDK or firmware update may change the behavior.
  Easy regression check: comment out the keepalive block, sideload,
  see if perf stays at 25 fps. If yes, the SDK fixed it and the
  block can be removed.
- If anyone figures out the actual mechanism, document it — knowing
  the *why* would let us shrink the keepalive block to a minimum.

## Perf-keepalive minimal recipe (2026-05-09, after ka1-ka5 bisection)

After shipping the original keepalive, did a controlled bisection to
identify which parts were actually load-bearing. Each test was its
own bundle (PokeMini KA1..KA5), Japanese Togepi first-three-levels,
measured via `getCurrentTimeMilliseconds` deltas in the log.

| Variant | Per-update | Per-second | fps |
| --- | --- | --- | --- |
| Baseline (clean) | nothing | nothing | ~12 |
| 1× / 4× `getElapsedTime` only | syscall | nothing | ~12 |
| `logToConsole("alive")` only | nothing | constant log | ~12 |
| ka1 (FP math + log) | math | log w/ args | ~20 |
| ka2 (log only) | nothing | log w/ args | ~22 |
| ka3 (log 5×/sec) | nothing | log w/ args ×5 | ~22 |
| ka4 (4× ge + log) | 4× ge | log w/ args | **~25** |
| ka5 (1× ge + log) | 1× ge | log w/ args | ~22 |
| Original keepalive (2× ge + math + log) | math + 2× ge | log w/ args | ~24 |

### What's actually load-bearing

1. **A format-rich `logToConsole` call once per second.** This alone
   gets +10 fps over baseline (12 → 22). Constant-string logs do
   nothing — the `%f`/`%.1f` format processing is what triggers
   whatever firmware activity helps.
2. **At least 4× per-update `getElapsedTime` calls** on top of the
   log. This adds another +3 fps (22 → 25). 1× syscall is not
   enough; somewhere between 1 and 4 the threshold is crossed (we
   didn't bisect 2 vs 3 — irrelevant for production).
3. Log frequency past once/sec: **does nothing**. ka3 (5×/sec) was
   no faster than ka2 (1×/sec), just more spam.
4. Per-update FP math: **negative**. ka1 (math + log) was ~2 fps
   *worse* than ka2 (log only). The original keepalive's float
   accumulator was overhead, not a contributor.

### What ships

The minimum effective recipe in `update()`:

```c
(void)pd->system->getElapsedTime();
(void)pd->system->getElapsedTime();
(void)pd->system->getElapsedTime();
(void)pd->system->getElapsedTime();

static int   keepalive_count = 0;
static int   keepalive_total = 0;
static unsigned int keepalive_t0 = 0;
keepalive_total++;
if (++keepalive_count >= 30) {
    /* once-per-second format-rich log with %f arg */
}
```

~25 fps on heavy Japanese Togepi (slightly better than the original
keepalive at 24 fps because the math overhead is gone). Same code
size as before, half the moving parts.

### Hypothesis on the firmware mechanism

We still don't know exactly why this works. Best guess based on the
data shape:

- The format-rich log triggers a firmware-side I/O path
  (format → USB serial write) that keeps some scheduler/clock state
  warm. Once-per-second is enough to "refresh" it.
- The 4× per-update `getElapsedTime` calls provide enough syscall
  density per update to keep the M7 from dropping to a low-power
  state during pure-userland frames. 1× isn't enough; 4× is.
- Per-update FP math doesn't trigger either mechanism — it's just
  CPU work that competes with the emulator. Hence the -2 fps
  penalty when added.

This is consistent with a low-power-mode / DVFS-like behavior at
the firmware level that's invisible from user-space, but treats
"frequent syscall traffic" + "periodic I/O activity" as the
heuristic for "this app is doing real work, give it full perf."

If this guess is right, a hypothetical `pd->system->keepActive()`
API in a future SDK would replace the whole block. Until then, the
recipe stays.

## ITCM correction (2026-05-09)

A previous note here claimed ITCM placement was the next big lever
toward native PM speed on heavy ROMs. **It isn't.**

The Playdate firmware does not expose the M7's actual ITCM hardware
to user code. The community calls "ITCM" what is in practice
elaborate `.text` placement: linker scripts that name sections
`.itcm.foo`, place them at specific addresses inside the regular
`.text` output (no `AT()`/separate LMA), and rely on ALIGN(0x1000)
offsets to manage branch-predictor aliasing on the M7. Confirmed by
inspecting [CrankBoyHQ/crankboy-app's link_map.ld](https://raw.githubusercontent.com/CrankBoyHQ/crankboy-app/development/link_map.ld) —
their `.itcm.dmg.*` sections live in regular `.text`, with no
runtime memcpy to a TCM region. Their own comment reads
`/* code to copy to TCM, seemingly runs faster */` — note the
"seemingly," and the absence of any actual copy.

So "ITCM" in the Playdate community is shorthand for "even more
aggressive linker tuning than what we already do." The technique
*does* yield small wins (we got ~5% from one alignment knob, see
below), but it cannot close the heavy-ROM gap to native speed.
The realistic path to native speed on the M7 is a computed-goto
rewrite of `MinxCPU_Exec` — see "Computed-goto rewrite" planning
below.

## Linker-tuning pass: 4 KB alignment of MinxCPU_Exec (2026-05-09)

After section consolidation got heavy Japanese Togepi to 81% real
PM speed, tried to push further with CrankBoy-style alignment
tuning. The observation: the M7 branch predictor table is keyed on
bits modulo 0x1000, so functions whose entry points hit the same
mod-0x1000 offset can alias each other in the predictor.

### Mechanism

- New `POKEMINI_HOT_EXEC` section attribute (`.text.hot.exec`),
  applied to `MinxCPU_Exec` only.
- `link_map.ld` places `.text.hot.exec` at `ALIGN(0x1000)` (4 KB
  boundary), then `.text.hot*` (the rest of the hot pack) right
  after with cache-line alignment.
- Net effect: `MinxCPU_Exec` is forced to mod-0x1000 offset `0x000`
  instead of the accidental `0x220` it had before.

### Result

| | Section consolidation only | + 4 KB alignment of MinxCPU_Exec |
| --- | --- | --- |
| Heavy Togepi steady display fps | ~24 | ~25 |
| Heavy Togepi PM frames/sec | ~58 | ~60-62 |
| ms per PM frame | 17.1 | 16.2 |
| % real PM speed | 81% | **86%** |

**~5 percentage points** on top of section consolidation, in line
with CrankBoy's reported gain magnitudes. Best windows now hit
93% real PM speed (vs. previously capped at ~83%), e.g.:

```
perf: 29 calls (28.2 fps), 69 PM frames in 1.03s | emu=937ms
```

Worst-case stalls (~10 fps, ~25 PM frames) are unchanged — those
are scene-driven busy-waits in the game, not addressable from
layout.

### Why this is the ceiling for layout tricks

We already have:
- Section consolidation (`.text.hot` first in `.text`)
- 4 KB alignment of the dispatcher
- `-Os -falign-loops=32` (no LTO, dropped for sections)
- Custom `link_map.ld` overriding the SDK's

CrankBoy goes further with multiple `ALIGN(0x1000); += <constant>`
incantations, sub-section partitioning of hot helpers, and dozens
of empirically-tuned offset constants. We could chase those, but
their reported gain is in the same 1-3% range as what we'd add.
**Diminishing returns.** The remaining 14% gap to native is not
addressable through layout.

## Computed-goto rewrite (next perf lever)

The dispatcher overhead itself is the next target. `MinxCPU_Exec`
is structured as a 256-case `switch` plus per-handler functions.
Every opcode dispatch involves: fetch IR, branch to switch top,
indirect branch through jump table to case body, run body, return
to outer loop, branch back to top. The per-instruction overhead
beyond the actual opcode work is meaningful — typically 5-10
M7 cycles per dispatch on a loop like this.

The computed-goto pattern (a GCC extension: labels-as-values, see
`&&label`) replaces switch dispatch with direct threaded code:

```c
static const void *const handlers[256] = {
    &&op_00, &&op_01, /* ... */ &&op_ff,
};
goto *handlers[fetch_opcode()];

op_00: /* opcode 00 body */
       goto *handlers[fetch_opcode()];
op_01: /* opcode 01 body */
       goto *handlers[fetch_opcode()];
/* ... */
```

Each opcode handler ends with its own `goto *handlers[...]` rather
than returning to a common switch top. Branches don't converge,
which lets the M7 predictor keep per-opcode state. Saves the
return + re-dispatch cycles per opcode.

### Plausible gain

Hard to predict precisely. Reported gains in the literature for
similar interpreter rewrites range from 10-30% on dispatch-bound
cores. M7 is dispatch-bound here (CPU phase = 62% of `emu` time),
so a 20% reduction in dispatch overhead would shave ~7% off the
total per-PM-frame cost. That'd put us in the ~91% real-PM-speed
range — close to native, possibly all the way there with the
current layout already in place.

### Risks / cost

- **GCC-only:** `&&label` and `goto *p` are GCC extensions. Other
  PokeMini ports (MinGW, Visual Studio sim, etc.) wouldn't compile.
  Solution: provide both implementations behind a `#if defined(__GNUC__)`
  guard, falling back to the existing switch on non-GCC.
- **Substantial code churn:** `MinxCPU_XX.c` (256-case switch) is
  the canonical place for this, but `MinxCPU_CE.c`, `MinxCPU_CF.c`,
  `MinxCPU_SP.c` (prefix-byte dispatchers) are smaller switches
  that would also benefit.
- **Maintenance:** the label table needs to stay in lockstep with
  the opcode set. Adding/removing opcodes means updating both the
  handler bodies and the table.
- **Upstream PR:** if it works, worth proposing back to JustBurn's
  PokeMini for inclusion under a `#ifdef PERFORMANCE_GOTO` flag.

### Approach when starting

1. Write the alternative `MinxCPU_Exec` body in `MinxCPU_XX.c`
   guarded by `#if defined(__GNUC__) && defined(PERFORMANCE)`.
2. Keep the existing switch as the fallback path.
3. Test: build, sideload, measure with the keepalive block.
4. If gain is real: do `MinxCPU_ExecCE`, `_ExecCF`, `_ExecSPCE`,
   `_ExecSPCF` similarly.
5. If gain is small/negative: revert; computed-goto isn't a fit
   here for whatever reason (maybe `MinxCPU_OnRead` memory latency
   dominates instead of dispatch overhead).

## Batch-dispatcher experiment — reverted (2026-05-09)

Tried a partial step toward computed-goto: refactored `MinxCPU_XX.c`
into a dual-mode source that emits either the original
`MinxCPU_Exec(void)` or a new `MinxCPU_ExecBatch(int target_cycles)`
behind `POKEMINI_BATCH_DISPATCH`. Batch mode wraps the existing
switch in a `while (cycles < target_cycles)` loop and inlines the
StallCPU + Status pre-amble checks per opcode, eliminating the
Hardware.c outer loop's per-opcode function call into
`MinxCPU_Exec`. `EXEC_RETURN(n)` macro abstracts the
return-vs-goto differences between the two modes.

### Implementation gotcha worth recording

First version of `EXEC_RETURN` used `do { cycles += n; continue; }
while (0)`. Bug: `continue` inside the trivial do-while terminates
*it*, then control falls through to the next switch case. The
intent was to escape to the outer `while (cycles < target_cycles)`,
but `continue` in C only escapes the innermost loop. After
emergency rebuild for the user, the symptom was the game running
at ~0.4 fps with each PM frame taking ~1.5 seconds (because each
`Fetch8()` triggered the entire sequence of opcode handlers from
that opcode onward).

Fix: use `goto exec_iter_end;` instead. `goto` cleanly bypasses
nested `do { ... } while (0)` and switch scopes to a single label
at the bottom of the loop body. This is the right pattern any time
you need a "continue outer loop" from inside macros that wrap their
body in trivial loops.

### Result on Japanese Togepi

| | Linker-tuning baseline | Batch dispatcher |
| --- | --- | --- |
| Steady-state fps | ~25 | ~23-25 |
| Steady-state PM frames/sec | ~60-62 | ~55-60 |
| Best-window PM frames | 69 (28 fps) | **72 (29.8 fps, real-time!)** |
| Worst-stall fps | ~9.9 | **1.5-4** |

Best windows hit real-time PM speed (a first), but average is no
better than the linker-tuning baseline and stalls are
**dramatically worse** (~2 fps vs ~10 fps). User also reported a
visual anomaly during the intro→gameplay transition (post-boot
sequence flashed for half a second after name input). Net: the
refactor introduced enough variance and possible subtle correctness
issues to be a clear regression, not an improvement.

### Why dispatch overhead wasn't the bottleneck

The hypothesis was that the Hardware.c → MinxCPU_Exec function
call per opcode dominated the dispatch cost. The data says
otherwise: each opcode does enough real work
(`MinxCPU_OnRead(1, …)` accesses to `PM_RAM` / `PM_ROM`,
`ADD8`/`AND8`/etc. with status-flag updates) that the function
call setup is a small fraction. Eliminating it earned us nothing
measurable on average and HURT us on heavy busy-waits — likely
because the per-opcode preamble (Shift_U, Status, StallCPU) now
runs every opcode instead of being skipped between calls when the
outer Hardware.c loop checked StallCPU first.

The actual bottleneck appears to be **memory-read latency on
PM_ROM accesses inside opcode handlers**, not dispatch overhead.
That's not addressable by interpreter-loop restructuring; it'd
need either DTCM-equivalent placement of PM_RAM (which the SDK
doesn't expose, same caveat as ITCM) or a fundamentally different
emulator architecture (JIT, dynarec — way out of scope).

### Where the work landed

- `source/MinxCPU_XX.c`: reverted to original `MinxCPU_Exec(void)`
  with `return N;` per case. Keeps the `POKEMINI_HOT_EXEC` tag
  (the linker-tuning win, which is independent and still active).
- `source/Hardware.c`: reverted to upstream `MinxCPU_Exec()` calls
  in both `PokeMini_EmulateCycles` and `PokeMini_EmulateFrame`. No
  `#ifdef POKEMINI_BATCH_DISPATCH` guards anymore.
- `source/MinxCPU.h`: reverted (no `MinxCPU_ExecBatch` declaration).
- `platform/playdate/CMakeLists.txt`: `POKEMINI_BATCH_DISPATCH`
  removed from compile definitions.

### Production ceiling

With every Playdate-friendly lever exercised (section
consolidation, 4 KB alignment of `MinxCPU_Exec`, `-Os` over `-O3`,
`-falign-loops=32`, custom linker script, perf-keepalive, the
single-opcode `MinxCPU_Exec`), the production ceiling on heavy
Japanese ROMs is **~86% real PM speed** (light scenes and English
ROMs at 100%). This is where things stay until either:
- A computed-goto rewrite of the *entire* dispatch chain (with
  per-opcode handlers chaining directly via `goto *`, eliminating
  the central switch entirely) is attempted — much bigger refactor
  than the batch experiment, with similarly uncertain payoff.
- A future Playdate SDK exposes ITCM/DTCM access, allowing the
  dispatcher and PM_RAM to live in 1-cycle on-chip memory.
- Someone with deeper Cortex-M7 expertise figures out an avenue
  none of us have considered.

## Performance levers (quick reference)

If perf regresses:
- `CommandLine.synccycles` — currently 512, max under `PERFORMANCE`.
  Lower = more accurate hardware sync, slower. UI.c clamps to 8..512.
  The emulator core itself accepts values higher than 512, but
  **don't** — the PRC frame trigger becomes unreliable above that
  and the screen renders white. See "what worked" step 5.
- **Compile flags** are `-Os -falign-loops=32` (no `-flto`) per-target
  in `CMakeLists.txt` (armgcc branch only). `-flto` was *removed*
  intentionally — see "Dispatcher locality pass" below for why.
- **Custom `link_map.ld`** in `platform/playdate/` puts `.text.hot`
  first in `.text`. Override the SDK's default by stripping its `-T`
  flag from `LINK_OPTIONS` and re-adding our own (CMakeLists has the
  pattern). Functions tagged `POKEMINI_HOT` (defined in `PokeMini.h`)
  go into `.text.hot`.
- `pd->display->setRefreshRate(30)` — stays at 30. PM at 72 Hz doesn't
  divide evenly into anything the Playdate panel comfortably runs.
- `CommandLine.lcdmode` — defaults to `LCDMODE_ANALOG`, exposed in the
  Playdate system menu as `LCD Mode: Soft`. This adds ~2-3% CPU vs
  `LCDMODE_2SHADES` (DecayRefresh runs every PRC frame) but is the only
  mode that gives smooth gray-suppression under motion. The menu's `Fast`
  option switches to raw `LCDMODE_2SHADES` for measurement; first
  subjective device test did not feel noticeably faster. The `Soft` label
  is intentionally short because Playdate option labels clip at roughly
  five characters.

Things **not** worth changing: the audio path (already callback-driven
like NDS), the `LCDDirty` early-return in `render_screen`, the 12/5
fractional pacing pattern.

### Fast instruction fetch experiment — reverted (2026-05-09)

Tried a Playdate-only `POKEMINI_FAST_FETCH` path in `Fetch8()` that
bypassed `MinxCPU_OnRead()` for the common cartridge-ROM instruction
fetch case (`addr >= 0x2100 && PM_ROM`) and fell back to the callback
for BIOS/RAM/I/O/no-cart behavior.

Result on Japanese Togepi, smooth LCD mode, first three levels:
average display rate was ~23.3 fps over 51 measured windows, with
steady windows mostly around 24-25 fps and scene stalls down to
~11.8/16.2/17.1 fps. That is not an improvement over the documented
post-linker-tuning baseline. The added inline branch/check also grew
`.text` by ~528 bytes, which may be enough to offset any saved callback
cost on the M7's small I-cache.

Conclusion: reverted. `MinxCPU_OnRead()` is already in the hot section
near the dispatcher; the function-call cost was not the remaining
bottleneck.

### Per-file optimization experiment: MinxPRC.c -O2 — reverted (2026-05-09)

Tried keeping the global device build at `-Os -falign-loops=32`, but
compiling only `source/MinxPRC.c` with `-O2`.

Result on Japanese Togepi, smooth LCD mode, first three levels:
average display rate was ~22.2 fps over 50 measured windows. Steady
sections sat mostly around 22.7-23.2 fps instead of the earlier 24-25 fps
band, with stalls down to ~13.3/16.8/16.9 fps. `.text` grew to 115190
bytes. Net regression.

Conclusion: reverted. PRC `-O2` likely bloats code enough to lose more
I-cache locality than it gains from local optimization.

### Per-file optimization experiment: MinxTimers.c -O2 — superseded (2026-05-09)

After PRC `-O2` regressed, tried keeping the global device build at
`-Os -falign-loops=32`, but compiling only `source/MinxTimers.c` with
`-O2`. Timer sync is a smaller hot path than PRC, so the code-size risk
is lower.

Result on Japanese Togepi, smooth LCD mode, first three levels:
average display rate was ~24.3 fps over 52 measured windows, with 28
windows at or above 25 fps and 8 at or above 26 fps. Steady sections
felt and measured smoother than the previous variants. `.text` grew only
to 112406 bytes, much smaller than the PRC `-O2` regression build.

Conclusion: this was the first positive per-file optimization, but
`MinxTimers.c -O3` slightly edged it out in the next A/B. Keep this result as
the fallback if the `-O3` result does not reproduce.

### Per-file optimization experiment: MinxTimers.c -O3 — kept in current stack (2026-05-09)

After `MinxTimers.c -O2` improved Togepi JP, tried keeping the global device
build at `-Os -falign-loops=32`, but compiling only `source/MinxTimers.c`
with `-O3`.

Result on Japanese Togepi, smooth LCD mode, first three levels:
average display rate was ~24.4 fps over 51 measured windows, with 31
windows at or above 25 fps and 5 at or above 26 fps. This is a tiny gain over
the `-O2` timer build (~24.3 fps, 28 windows >=25 fps, 8 windows >=26 fps),
but it is directionally positive and `.text` only grew to 112438 bytes.

Conclusion: keep `MinxTimers.c -O3` in the optimization stack. It was the
current best by itself, then `Hardware.c -O2` improved the stack further.

### Per-file optimization experiment: Hardware.c -O2 — kept (2026-05-09)

After `MinxTimers.c -O3` became the current keeper, tried keeping the global
device build at `-Os -falign-loops=32`, keeping `source/MinxTimers.c` at
`-O3`, and additionally compiling only `source/Hardware.c` with `-O2`.

Rationale: `Hardware.c` owns the outer PM frame execution loop and calls
`MinxTimers_Sync()` plus `MinxPRC_Sync()` repeatedly. It is much smaller than
the CPU and PRC translation units, so this may improve the control loop without
the larger I-cache/code-size risk seen with `MinxPRC.c -O2`.

Result on Japanese Togepi, smooth LCD mode, first three levels:
average display rate was ~25.9 fps over 52 measured windows, with 37
windows at or above 25 fps, 36 at or above 26 fps, and 27 at or above 27 fps.
This is a clear improvement over the prior `MinxTimers.c -O3` keeper
(~24.4 fps, 31 windows >=25 fps). `.text` grew only to 112694 bytes, +256
bytes over the timer-only `-O3` build.

Conclusion: keep `Hardware.c -O2` together with `MinxTimers.c -O3`. The next
experiment should be chosen cautiously: the broad/hot files can regress from
I-cache pressure, as `MinxPRC.c -O2` already showed.

### Per-file optimization experiment: Hardware.c -O3 — reverted (2026-05-09)

After `Hardware.c -O2` clearly improved Togepi JP, tried keeping the global
device build at `-Os -falign-loops=32`, keeping `source/MinxTimers.c` at
`-O3`, and changing only `source/Hardware.c` from `-O2` to `-O3`.

Rationale: `Hardware.c` is small enough that the extra `-O3` code growth may be
acceptable, unlike the larger PRC translation unit. This is a direct A/B
against the current best stack.

Result on Japanese Togepi, smooth LCD mode, first three levels:
average display rate was ~25.4 fps over 53 measured windows, with 36
windows at or above 25 fps, 32 at or above 26 fps, and 11 at or above 27 fps.
This remained better than the timer-only stack, but regressed against
`Hardware.c -O2` (~25.9 fps, 37 windows >=25 fps, 36 windows >=26 fps, 27
windows >=27 fps). `.text` grew to 112758 bytes, only +64 bytes over
`Hardware.c -O2`, so the regression is probably code-shape/scheduling rather
than meaningful size pressure.

Conclusion: reverted to `Hardware.c -O2`. Keep the current stack as
`MinxTimers.c -O3` plus `Hardware.c -O2`.

### Temporary phase diagnostic build — completed (2026-05-09)

Diagnostic after the per-file optimization stack landed. The diagnostic build
defined `PD_PERF_DIAG`, which adds timing probes around:
`PokeMini_EmulateFrame()`, the CPU execution chunk in `Hardware.c`,
`MinxTimers_Sync()`, `MinxPRC_Sync()`, `MinxAudio_Sync()`, input, render,
and outer update/misc time.

Logs still include the normal `perf:` keepalive line. The diagnostic build
also prints one `diag:` line every 30 Playdate updates:

```text
diag: upd=30 pm=72 total=...us emu=... cpu=... tim=... prc=... aud=... input=... render=... misc=...
```

Result on Japanese Togepi, smooth LCD mode, first three levels: diagnostic
overhead dropped display fps to ~18-19, so use the relative phase split only.
Across 48 diagnostic windows, and more importantly the steady windows after
startup/outliers, the shape was:

| Phase | Share of total update time | Share of emulation time |
| --- | ---: | ---: |
| Emulation wrapper total | ~98.7% | - |
| CPU dispatch chunk | ~67-68% | ~68-69% |
| PRC sync | ~15-16% | ~16% |
| Timers sync | ~8% | ~8% |
| Render | ~1% | - |
| Input/misc/audio | ~0-1% | ~0-8% unaccounted loop/instrumentation overhead |

Conclusion: the remaining bottleneck is still overwhelmingly
`MinxCPU_Exec()`/dispatch. Playdate-side render, input, and audio are not
worth chasing next. `PD_PERF_DIAG` is now disabled in CMake for the normal
keeper build, but the guarded probes can stay in source for future diagnostics.

### Per-file optimization experiment: MinxCPU_XX.c -O2 — reverted (2026-05-09)

After the phase diagnostic confirmed CPU dispatch still dominates, tried
keeping the current keeper stack (`MinxTimers.c -O3`, `Hardware.c -O2`,
global `-Os -falign-loops=32`) and additionally compiling only
`source/MinxCPU_XX.c` with `-O2`.

Rationale: `MinxCPU_XX.c` contains the main 256-opcode `MinxCPU_Exec`
dispatcher. This is the largest remaining hot bucket, but also the riskiest
file to expand because the M7 Rev A has only a 4 KB I-cache. This A/B checks
whether GCC's `-O2` scheduling is worth the likely code growth before doing a
computed-goto rewrite.

Build note: `.text` grew to 122934 bytes, roughly +10 KB over the current
keeper's 112694 bytes. That is a large expansion, so watch for I-cache
regression despite the dispatcher-local optimization.

Result on Japanese Togepi, smooth LCD mode, first three levels:
average display rate was ~23.6 fps over 50 measured windows, with only 6
windows at or above 25 fps. This regressed badly against the keeper stack
(`Hardware.c -O2` + `MinxTimers.c -O3`, ~25.9 fps with 37 windows >=25 fps).

Conclusion: reverted. The main dispatcher is too sensitive to code growth for
a per-file `-O2` flag. The next meaningful path is source-level dispatch work
(computed-goto or similarly locality-aware restructuring), not broader
optimization flags on `MinxCPU_XX.c`.

## LCD shading / dither suppression (2026-05-03)

PM games fake gray by toggling pixels every native frame (72 Hz). Real
PM hardware blurs the flicker into perceived gray via slow LCD response;
Playdate's fast 1-bit panel doesn't, so without intervention the player
sees raw flicker. The journey through five approaches below; the final
one (LCDMODE_ANALOG with thresholded checkerboard) is what's in the code.

Quick pick: if you're tweaking, the threshold values `t_off` and `t_on`
in `render_screen` are the main knobs (currently 3/8 and 5/8 of the
contrast span). Tightening the gap = less dither, more "binary"; widening
= softer fades, more dither.

The Playdate system menu exposes this as `LCD Mode: Soft/Fast`:
- `Soft` (default) = `LCDMODE_ANALOG` + thresholded checkerboard.
- `Fast` = `LCDMODE_2SHADES` + direct `LCDPixelsD` blit, kept as a
  measurement toggle even though the first device test did not produce an
  obvious perceived speedup.

### Approaches tried, in order

1. **`LCDMODE_2SHADES`, raw current frame** (the original port). Just
   render `LCDPixelsD[i]`. Pixels flicker visibly because that's literally
   what the game writes.
2. **`LCDMODE_3SHADES`, OR current+previous**. Pixel renders on if
   either current or previous PRC frame had it lit. Dithered "gray"
   regions become solid filled — too dark, looks like everything is
   "always on."
3. **`LCDMODE_3SHADES`, AND current+previous**. Pixel renders on only
   if both frames had it lit. Dithered "gray" regions disappear entirely
   — sprites that intentionally flicker for shading become invisible.
4. **`LCDMODE_3SHADES`, checkerboard for flicker** (good for static).
   `pixelOn = both | (either & dither)`. Steady-on stays on, steady-off
   stays off, flickering pixels render in a row-alternating checkerboard
   pattern (mask 0xAA / 0x55 by row parity). Static gray fills look
   convincing — the spatial dither reads as gray. **But moving gray
   sprites smear**: leading edge (off→on) and trailing edge (on→off)
   both register as "flickering" and pick up dither, while the dither
   pattern itself is fixed to screen position so it appears to crawl
   across the moving sprite.
5. **`LCDMODE_ANALOG`, thresholded checkerboard** (final). The emulator
   keeps a 4-bit per-pixel history (`LCDPixelsAS`, MinxLCD.c:222–223) and
   `MinxLCD_DecayRefresh` (MinxLCD.c:215) writes a 5-level smoothed value
   into `LCDPixelsA` interpolated between the game's contrast intensities.
   `render_screen` computes `t_off` and `t_on` from those intensities each
   call (3/8 and 5/8 of the span — boundaries at level 1.5 and 2.5):
   - A ≥ t_on  → solid on  (lit ≥3 of last 4 PRC frames)
   - A ≥ t_off → checkerboard (level 2)
   - else      → solid off

   Moving gray sprite: leading edge needs 2 frames to cross t_off, so it
   fades in instead of dithering. Trailing edge fades out through dither
   to off over 3 frames. Reads as motion blur instead of dither smear.
   Static gray still hits level 2 → dither → unchanged from approach 4.

### Why thresholds adapt to contrast

`MinxLCD.Pixel0Intensity` and `Pixel1Intensity` are *not* fixed at 0/255
— they're table-driven by the game's contrast register
(MinxLCD_ContrastLvl, MinxLCD.c:531). At low contrast both values
collapse toward the middle (e.g. `{240, 255}` at register 0x37+), so
hardcoded byte thresholds like 64/192 stop working. Computing `t_off`
and `t_on` from the live `Pixel0Intensity`/`Pixel1Intensity` keeps the
3-bucket split correct regardless of what the game does.

### Performance cost

`MinxLCD_DecayRefresh` runs ~6144 iterations per PRC frame at 72 Hz,
roughly 60K instructions × 72 = 4.3M instr/sec ≈ ~2-3% of CPU. Light
scenes still hit 100% real speed; heavy scenes drop from 93% to ~91%.
`render_screen` itself is the same shape (still 2 packs per byte +
some bitwise ops), no measurable change vs approach 4.

### What was deliberately not done

- **Per-frame mode switching** (e.g. 2-shade during boot, ANALOG in-game).
  Tried it: detect "boot done" by waiting for `MinxCPU.PC.D >= 0x2100`
  (freebios.asm:187 does `jmp @00002102` to hand off). Boot rendered
  cleanly but the transition wasn't worth the complexity — reverted.
- **Multi-level dither** (different dither densities per brightness band).
  Would give smoother gradients, requires per-pixel position-dependent
  threshold lookups (Bayer matrix), breaks the byte-pack speedup. Not
  attempted — current 1-bucket dither already looks good.
- **Wider history** beyond 4 frames. The PM's dither cycles at 2-frame
  period; 4 is enough to distinguish steady gray from transient. Going
  longer makes motion ghost more without improving steady-state look.

## Save state / EEPROM

EEPROM save uses `fopen`, which works on simulator but silently fails on
device. To save EEPROM on hardware, set `PokeMini_CustomSaveEEPROM` to a
function that uses `pd->file->*` APIs (write to `~/Data/<game>/<rom>.eep`).
Not implemented yet.

## Open questions

- Why did the simulator show a white screen on the *first* clean build but
  work after a second clean rebuild? Suspect stale `pdex.dylib` cached by
  the simulator process or a build-system quirk where `cmake ..` + `make`
  doesn't fully rebuild on a freshly-created directory. Using
  `cmake --build .` instead of `make` may be more reliable.
