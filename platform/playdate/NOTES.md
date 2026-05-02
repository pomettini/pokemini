# PokeMini Playdate Port — Development Notes

Living document of bugs found, fixes applied, and gotchas learned while porting
the PokeMini emulator to the Playdate. Update this whenever something
non-obvious is discovered. Newest entries on top.

## Build commands

```
rm -rf build build-device PokeMini.pdx PokeMini_DEVICE.pdx
mkdir build && cd build && cmake .. && make && cd ..
mkdir build-device && cd build-device && \
    cmake .. -DTOOLCHAIN=armgcc \
        -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake && \
    make && cd ..
```

`PokeMini.pdx/pdex.dylib` is the simulator binary. `PokeMini_DEVICE.pdx/pdex.bin`
is the device binary. Upload `PokeMini_DEVICE.pdx` to the device.

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
  `build/` leaves a stale `PokeMini_DEVICE.pdx` that can mask new fixes.
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

## Performance levers

If the game runs slow:
- **Increase `CommandLine.synccycles`** — currently 16. Try 32 or 64. Higher
  values reduce PRC/Timer sync overhead at the cost of slightly less accurate
  interrupt timing. 64 is the documented max for non-sound-sync ports.
- **Re-enable LCDDirty early-return** in `render_screen` (already enabled).
- **Compile with `-O3`** (currently `-O2` per Playdate's `arm.cmake`).
- **Consider `setRefreshRate(50)` with 1 emulated frame per call** if 30fps
  with 2 frames is too much. PM is natively 72Hz; either 60Hz (current) or
  50Hz emulation will look wrong but be playable.

## Performance analysis — comparing to other ports (2026-05-02)

Currently running well below native Pokémon Mini speed on device. The NDS
port runs at full speed on a 67 MHz ARM9, so a 180 MHz Cortex-M7 should
have plenty of headroom — the slowness is something the port is doing,
not a hardware ceiling. What follows is a comparison against ports that
hit native speed and a list of suspected bottlenecks ranked by severity.

### What the fast ports do

**NDS (`platform/nds/PokeMini_NDS.c`, `platform/nds/makefile:37`)** —
runs full-speed on weaker hardware:
- `CFLAGS += ... -DPERFORMANCE` (makefile:37). With `PERFORMANCE` defined,
  `CommandLineInit` sets `synccycles = 64` (CommandLine.c:118). The NDS
  port does **not** override it, so it runs with the full 64-cycle batch.
- Uses **`Video_x2`** at 192×128 with a custom **8-bit paletted** variant
  `PokeMini_Video2x2_8P` (PokeMini_NDS.c:36, 361). Output is one byte per
  destination pixel — no per-pixel format conversion in the port.
- Audio uses the Maxmod stream callback (PokeMini_NDS.c:210–214) calling
  `MinxAudio_GenerateEmulatedS8` directly into the destination buffer.
  Same `POKEMINI_GENSOUND` callback model the Playdate already uses.
- Renders only when `LCDDirty` is set (PokeMini_NDS.c:424–427) — same
  optimization the Playdate has.

**Other small ports**: GCW uses `Video_x3` (288×192, 16-bit), BitBoy and
nspire use `Video_x2`, PSP uses `Video_x2`/`Video_x4`. None of them
override `synccycles`; they all rely on the `PERFORMANCE` default of 64.

### Where the Playdate port is leaving cycles on the floor

In rough order of suspected impact:

**1. `synccycles = 16` is below the `PERFORMANCE` default of 64**
(`PokeMini_Playdate.c:263`). Every sync exits the inner CPU loop, calls
`MinxTimers_Sync()` and `MinxPRC_Sync()` (Hardware.c:55–57). At 16 vs 64
that's roughly **4× more sync calls per frame**. (Earlier estimates of
"3500×" were wrong — they confused `synccycles` with `POKEMINI_FRAME_CYC`,
which is the total cycles per frame, not the sync granularity.) Cheap fix:
bump to 64 and measure. UI clamps the upper bound to 512 in `PERFORMANCE`
mode (UI.c:915), so 128 or 256 are also worth trying.

**2. `render_screen` does scalar bit-packing on the CPU**
(`PokeMini_Playdate.c:193–222`). Every refresh:
- 64 rows × 12 bytes × 8 conditional ORs = **6144 compares + ORs** to
  pack `LCDPixelsD` (1-byte-per-pixel) into 1-bpp.
- Then 64 rows × 12 bytes × 3 (scale) × 3 (vertical repeat) = **6912
  byte writes** through a 256-entry LUT into the framebuffer.

The bit-packing loop is the suspicious part — eight branchy `if (src[i] == 0) byte |= 0x..` per byte. Two cheap rewrites worth trying (in order):

- Branchless pack: `byte = ((src[0]==0)<<7) | ((src[1]==0)<<6) | ...` —
  lets the compiler emit conditional selects instead of branches.
- Read 8 bytes as `uint64_t`, compare equal to 0 lane-wise, extract bits.
  The Cortex-M7 doesn't have NEON but it has `UQSUB8`/`USUB8`/`SEL` and
  a 64-bit load (`LDRD`) — should pack 8 pixels in a handful of insns.

A more invasive option: write a custom 1-bpp Video renderer (analogous
to NDS's `_8P` variant) so the emulator core writes directly into a
1-bit buffer. That removes the pack step entirely. The Playdate is
400×240 1-bit; the existing 3× scale (96×3=288 wide, 64×3=192 tall) is
fine. A 4× would be 384×256 — too tall.

**3. SDK build flags are stock `-O2`, no LTO, no Cortex-M7 tuning**
(`platform/playdate/CMakeLists.txt:84–90` only sets `-D` flags, the rest
comes from `$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake`). The SDK's
arm.cmake uses `-O2 -mthumb -mcpu=cortex-m7 -mfloat-abi=hard
-mfpu=fpv5-sp-d16` — the CPU/FPU flags are already correct, but `-O2`
without LTO is conservative for a tight emulation loop. Adding `-O3 -flto`
(per-target, not editing the SDK file) is a low-effort experiment.

**4. ITCM/DTCM placement of the CPU dispatch**. The Cortex-M7 on the
Playdate has tightly-coupled memory; running `MinxCPU_Exec` and the
opcode tables out of TCM avoids flash wait states. Need to check whether
the SDK exposes a `__attribute__((section(...)))` for this. Lower
priority than #1–#3 because flash on the Playdate is reasonably fast.

### Plan if revisiting perf

In order of effort/impact:
1. Set `synccycles = 64`, rebuild device, eyeball framerate. (1-line change.)
2. Branchless bit-pack in `render_screen`. (10-line change.)
3. Add `-O3 -flto` for the device target in CMakeLists. (2-line change.)
4. Custom 1-bpp Video renderer (`Video_x1_1bit.c` or similar) so the
   emulator writes a packed buffer directly. (Bigger change; the NDS
   `_8P` variant is the template.)
5. ITCM placement for `MinxCPU_Exec` and dispatch tables.

Things **not** worth changing: the audio path (already using the same
callback model as NDS), the `LCDDirty` early-return (already in place),
the 30fps × 2-frames-per-update cadence (PM is 72Hz, this is the
closest approximation without splitting frames).

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
