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
