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
- **ITCM/DTCM placement.** Playdate SDK's `link_map.ld` defines no TCM
  region. User code runs from internal SRAM (loaded ELF); ITCM is reserved
  for firmware. Section attributes `__attribute__((section(".itcm")))`
  won't be honored by the loader. Cortex-M7 TCM tricks don't apply here.
- **Native 1-bpp Video renderer** (in the `Video_x*.c` sense). The
  PokéMini renderer architecture reads from `LCDPixelsD` (1 byte per
  pixel, written by MinxLCD/MinxPRC) and produces output in some format.
  A "1-bpp variant" would still read 1 byte/pixel and pack — exactly what
  `render_screen` already does. Truly skipping `LCDPixelsD` would mean
  rewriting MinxLCD's per-pixel writes to RMW-pack bits, which is
  invasive and probably slower on Cortex-M7 (RMW vs byte store).

### Where the remaining 7% deficit lives

Heavy-scene saturation is 100% emulator-core-bound. To close it would
need real engineering inside the core, in approximate effort/impact order:
- Per-component profiling (split `MinxCPU_Exec` vs `MinxPRC_Sync` time)
  to confirm whether PRC sprite-render is the actual hotspot.
- Computed-goto / threaded-interpretation rewrite of `MinxCPU_Exec`'s
  256-case switch (currently a jump table — probably hard to beat with
  GCC).
- Frameskip option (drop every Nth PM frame's render under load, but
  keep emulating) — would help display smoothness, not real-time speed.

Probably not worth pursuing for a 7% deficit that only shows up briefly.

## Performance levers (quick reference)

If perf regresses:
- `CommandLine.synccycles` — currently 512, max under `PERFORMANCE`.
  Lower = more accurate hardware sync, slower. UI.c clamps to 8..512.
  The emulator core itself accepts values higher than 512, but
  **don't** — the PRC frame trigger becomes unreliable above that
  and the screen renders white. See "what worked" step 5.
- `-O3 -flto` is set per-target in `CMakeLists.txt` (armgcc branch only).
  Removing it costs ~10% emulator throughput.
- `pd->display->setRefreshRate(30)` — stays at 30. PM at 72 Hz doesn't
  divide evenly into anything the Playdate panel comfortably runs.
- `CommandLine.lcdmode` — currently `LCDMODE_ANALOG`. Adds ~2-3% CPU vs
  `LCDMODE_2SHADES` (DecayRefresh runs every PRC frame) but is the only
  mode that gives smooth gray-suppression under motion. See next section.

Things **not** worth changing: the audio path (already callback-driven
like NDS), the `LCDDirty` early-return in `render_screen`, the 12/5
fractional pacing pattern.

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
