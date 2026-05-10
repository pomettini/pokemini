# PokeMini Playdate — Path to Shippable

What's between the current "works on my device" state and a release that
others would actually use. Roughly ordered by ship-blocker → polish.

The emulator core (CPU, PRC, audio, video) and the timing layer are done.
Everything below is integration, UX, and distribution work.

## ROM storage convention

ROMs on device are loaded from **`/Shared/Emulation/pm/games/`**, following
CrankBoy's cross-emulator convention. The Playdate firmware exposes a
top-level `/Shared/` folder accessible to all games (added in SDK 2.4,
mkdir/stat/etc fixed in 2.4.1). The app `mkdir`s the path tree on first
launch so a fresh install lands the user in a known, writable location
they can sideload ROMs into.

When opening files there, use `kFileRead | kFileReadData` — `kFileRead`
alone only searches the read-only pdx bundle and silently misses
everything in `/Shared/`.

If `/Shared/Emulation/pm/games/` is empty, the emulator boots with
FreeBIOS only; FreeBIOS already shows a "no ROM" splash, which is a
better default than a custom homebrew demo.

EEPROM saves and save states stay in the app's own
`/Data/com.pomettini.pokemini/` — those are emulator-private and should
not be shared.

## 1. Ship blockers (cannot release without)

### 1a. Remove bundled ROM — done
`platform/playdate/Source/` previously contained `game.min` and
`lunch_time.min` (copyrighted Pokémon Mini cartridges). Those are gone.
`boot.min` (homebrew demo) is no longer used — when no user ROM is
present we boot FreeBIOS, which has its own splash that's nicer than
the demo. The file can stay in `Source/` for ad-hoc developer testing
or be deleted entirely; the C code no longer references it.

Remaining sub-task: add `Source/*.min` to `.gitignore` so casual local
ROM copies don't get committed.

### 1b. EEPROM save/load on device
`PokeMini_Playdate.c:390` notes that EEPROM save uses `fopen` (works on
simulator, silent fail on device). `load_rom` already uses `pd->file->*`
APIs correctly — the same pattern needs to wire into PokeMini's
`PokeMini_CustomSaveEEPROM` / `PokeMini_CustomLoadEEPROM` callback hooks.
Path scheme: `/Data/com.pomettini.pokemini/eeprom/<rom_basename>.eep`.

Without this, players lose all in-game progress whenever the app exits
— ship-blocker for any RPG-style game.

### 1c. ROM picker — done (rough)
Implemented in `PokeMini_Playdate.c`:
- mkdir cascade for `/Shared/Emulation/pm/games/` on boot.
- `*.min` enumeration via `pd->file->listfiles`.
- 0 ROMs: boot FreeBIOS only (its own no-cart splash).
- 1 ROM: auto-load it (no point showing a one-item picker).
- 2+ ROMs: simple D-pad + A list picker.
- System menu adds a "ROM Picker" item that returns to the picker
  mid-game without quitting the app.

Still rough; remaining work is mostly polish:
- Empty-state hint with the device path printed (currently silent — see
  §3b).
- The picker text uses a single system font and basic black/white
  rows; no scroll indicator, no thumbnails. Fine for testing.
- ~~Returning to the picker doesn't reset `MinxAudio` state, so a tone
  may briefly persist if the previous ROM was emitting one.~~ Fixed:
  `audio_callback` now emits silence when `app_mode != MODE_EMULATOR`.
- ~~ROMs with long filenames (e.g. fan-translation patches) were
  silently dropped from the picker.~~ Fixed: `MAX_ROM_NAME` bumped to
  128, dropped entries now log an explicit reason.

### 1d. License + attribution — partially done
- `Source/LICENSE` added: full GPLv3 text with a header crediting
  JustBurn (PokeMini core), Giorgio Pomettini (Playdate port), and
  Team Pokeme (freeBIOS, freeware/public domain). `pdc` bundles it
  into the `.pdx` automatically.

Still pending: a **Credits screen** in-app (or About entry in the
system menu) so users see attribution without having to open the
`.pdx` directory. Lightweight version is just an `addMenuItem("About",
...)` that draws the same credits text on a frame and waits for B
to dismiss.

### 1e. Card art and pdxinfo polish — partially done
Metadata (`name`, `author=Pomettini & JustBurn`, `description`,
`bundleID=com.pomettini.pokemini`, version) is now correct.

Still pending — the graphical assets (work in progress):
- A 350×155 launch card image (`launchImage` in pdxinfo).
- A 32×32 menu icon.
- Wire up `imagePath=` once the card art is finalized.

## 2. Important UX (would feel broken without)

### 2a. C button mapping — first pass done
The Pokémon Mini has A, B, **C**, D-pad, Power, Shake = 9 inputs.
Playdate gives us A, B, D-pad, Crank, accelerometer = 7 inputs total.
For now, C is synthesized as a held **crank angle zone** in
`PokeMini_Playdate.c::handle_input()`: undock the crank and rotate into
60°-180° to hold C; move outside the zone or dock it to release C. It bypasses
the saved joystick mapping table so existing configs where C was unmapped
still get the input.

Longer-term options:
- **Crank-pressed-down** (Playdate's `pd->system->isCrankDocked`): use
  crank rotation for one thing, docked state as a chord. Awkward.
- **A+B chord**: tried and rejected for now; it did not work reliably enough
  for Pinball testing and conflicts with any game that uses A+B.
- **Move "Shake" off the crank to the accelerometer** (see 2c) and put
  C on crank state/rotation. Current first pass uses a crank angle hold zone.
- **In-app remappable controls** (see 2d). Best long-term answer.

### 2b. Pause on Playdate system menu — done
The Playdate runtime stops calling `update()` when the system menu is
open (emulation naturally freezes). Two additions wired up in
`eventHandler`:
- `kEventPause`: sets `emu_paused = 1` and releases every held PM key
  (`MINX_KEY_A` through `MINX_KEY_SHOCK`) so no button stays stuck on
  resume.
- `kEventResume`: clears `emu_paused = 0`.
- `audio_callback` checks `emu_paused` alongside `app_mode` and emits
  silence while the menu is up.

`kEventLowPower` (charger unplugged near zero battery) is not yet
handled — low priority since the OS manages the shutdown sequence.

### 2c. Accelerometer-based shake — done
Shake is now driven by `pd->system->getAccelerometer`: squared-
magnitude past 2.5 g² (rest is ~1.0 g² thanks to gravity) emits a PM
Shake pulse, with a 6-frame cooldown so one gesture doesn't fire
multiple events. The accelerometer peripheral is enabled in
`kEventInit` via `setPeripheralsEnabled(kAccelerometer)`.

The crank is now unused — frees it up for the C button per 2a.

Threshold tuning is by feel; if it triggers too easily on regular
movement, raise `mag2 > 2.5f` in `handle_input`. If it needs a
violent shake to register, lower it.

### 2d. In-app settings menu
Use Playdate's system menu API (`pd->system->addMenuItem`,
`addOptionsMenuItem`) to expose:
- **Reset game** (PokeMini_Reset)
- **Audio**: on / off (toggle CommandLine.sound, mute the audio source)
- ~~**LCD mode**~~ — wired as `Soft` / `Fast` in the Playdate system menu.
  `Soft` is the default ANALOG smoothing path; `Fast` switches to raw
  `LCDMODE_2SHADES` for performance testing. The short label is intentional:
  Playdate option labels clip after roughly five characters.
- **C button**: which input does C map to (see 2a)
- **Save state** / **Load state** slots (see 2e)
- ~~**Quit to ROM picker**~~ — wired up (see 1c).

### 2e. Save states (in addition to EEPROM)
PokéMini already has full save/load state code (`POKELOADSS_*` /
`POKESAVESS_*` in every component). Wire it up:
- Bind to system menu items: "Save state slot 1/2/3", "Load state slot
  1/2/3".
- Path: `/Data/com.pomettini.pokemini/states/<rom_basename>.s1` etc.
  (emulator-private — not in `/Shared/`).
- Include a thumbnail in each save (optional; can just be a screenshot).

## 3. Quality polish (release-grade feel)

### 3a. Per-ROM data isolation
Currently EEPROM and save states (when added) live in flat directories.
Better: `/Data/com.pomettini.pokemini/<rom_basename>/eeprom.eep`,
`.../states/`, etc. Multiple ROMs with the same internal name don't
clobber each other.

### 3b. Empty-state UI
With the `boot.min` fallback in place, the app no longer black-screens
when `/Shared/Emulation/pm/games/` is empty — but the user should still
be told *why* they're seeing the demo ROM and how to load real games:
- When falling back to bundled `boot.min`, surface a small hint
  ("Drop `.min` ROMs into `/Shared/Emulation/pm/games/`") via a one-shot
  toast or About-screen entry. Don't be intrusive; the demo is playable.
- Don't crash on a bad ROM in that directory either — see 3d.

### 3c. Background/foreground audio handling — done
- Audio source torn down on `kEventTerminate` (was already in place).
- `kEventPause` / `kEventResume` now mute and unmute via the
  `emu_paused` flag checked inside `audio_callback` (see §2b).

### 3d. Error recovery
- Corrupt ROM (size 0, bad magic): show error, return to picker.
- Failed `malloc` for ROM buffer: same.
- Failed save state write: show error, don't claim success.

### 3e. Battery / contrast pass-through
PM has a low-battery indicator and contrast control. Could:
- Mirror Playdate battery state into PM's `low_battery` flag so games
  that show a low-battery icon do so when the Playdate is low.
- Add a contrast slider in the settings menu (the emulator already
  responds — see MinxLCD_SetContrast).

### 3f. Input feedback / latency
The current input mapping pushes events on press/release transitions
which is correct. Worth verifying there's no perceptible input lag from
the 30 fps display + PM 72 fps emulation cadence (one missed Playdate
frame = ~33ms = 2.4 PM frames late). If users complain, time-stamp the
button events and replay them at the right PM frame inside the
fractional-pacing loop.

### 3g. 3.5x scaling experiment — first pass done
The Playdate system menu now has `Scale: 3x/3.5x`. `3x` is the stable baseline
integer scale at 288x192. `3.5x` expands the 96x64 Pokemon Mini framebuffer to
336x224 with an alternating 3/4-pixel row/column pattern, centered on screen. This is
still an experiment: inspect readability, shimmer, and performance on device
before treating it as final.

## 4. Code/build hygiene

### 4a. Remove the lingering test ROM — done
`Source/game.min` and `Source/lunch_time.min` are gone from the tree.
`Source/boot.min` is no longer loaded by the C code (see 1a) — it can
be removed in a follow-up cleanup or kept for ad-hoc dev testing.

Follow-up: confirm `.gitignore` ignores `*.min` so casual local copies
of copyrighted ROMs don't get committed.

### 4b. README for the platform — done
`platform/playdate/README.md` covers:
- What this is, what works, ROMs-not-included disclaimer.
- Install (sideload via play.date/account or Mirror).
- Adding ROMs (`/Shared/Emulation/pm/games/`).
- Controls table.
- Known limitations and a pointer to this roadmap for the full list.
- Build instructions for developers (links NOTES.md).
- Credits + GPLv3 reference.
- An AI disclosure paragraph at the end.

Same file is meant to double as the itch.io page description.

### 4c. CI for the device build
The `build-device/` cmake config is fragile (PLAYDATE_SDK_PATH must be
exported, see project memory). A small CI script that builds both the
sim `.pdx` and device `.pdx` on every commit would catch regressions.
Probably out of scope unless a public repo.

### 4d. Strip unused code paths
`PokeMini_Playdate.c` still includes `Video_x1.h` and calls
`PokeMini_SetVideo(...)` even though `render_screen` reads `LCDPixelsD`/
`LCDPixelsA` directly and never invokes the resulting renderer function
pointer. Could remove the SetVideo call entirely and shave a few KB.

## 5. Out of scope (deliberately not doing)

- **Multi-cart support** — PM had multi-game cartridges; the upstream
  emulator supports them via `-multicart`. Niche, not worth the UI cost.
- **Network/IR communication** — PM had IR for trades. Playdate has no
  IR. Cannot emulate.
- **Color PRC mode** — `LCDMODE_COLORS` is a homebrew-only PM extension.
  No commercial PM games use it; no point on a 1-bit panel anyway.
- **Closing the heavy-scene emulation deficit** — heavy-scene ceiling
  is ~14% below native on the heaviest ROMs (Togepi), covered in
  NOTES.md. Not a ship blocker (most players won't notice; the games
  that hit it worst are the most demanding ones, where slight slowdown
  is plausible even on real hardware). Smooth+3.5x is within ~2 fps of
  fast+3x after the memory-bandwidth render pass (also in NOTES.md).

## Suggested order

A reasonable shipping path:
1. ~~**1a, 4a**~~ — copyrighted ROMs removed; `boot.min` retired in
   favor of FreeBIOS. ✅
2. ~~**1c**~~ — ROM picker scanning `/Shared/Emulation/pm/games/`. ✅
3. ~~**1d (LICENSE), 4b**~~ — LICENSE bundled, README done. ✅
4. ~~**2c**~~ — accelerometer-based shake (crank now free). ✅
5. **1b** — EEPROM persistence (a few hours, biggest player-facing win)
6. ~~**2b, 3c**~~ — pause/resume handling. ✅
7. **3a, 3b** — per-ROM data isolation + empty-state hint (half day)
8. **2d** — remaining settings menu wiring (audio/reset/save-state items;
   LCD mode and ROM picker are already wired; C has a crank first pass)
9. **2e, 1e** — save states UI + card art when graphics are ready
   (half day)
10. **1d (Credits)**, **3d, 3e, 3f** — in-app credits + polish pass
    (as time allows)
11. **3g** — review 3.5x scale readability/performance (very low priority)

That's roughly 1-2 days of focused work to a v1.0 release from here.
