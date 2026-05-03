# PokeMini Playdate — Path to Shippable

What's between the current "works on my device" state and a release that
others would actually use. Roughly ordered by ship-blocker → polish.

The emulator core (CPU, PRC, audio, video) and the timing layer are done.
Everything below is integration, UX, and distribution work.

## 1. Ship blockers (cannot release without)

### 1a. Remove bundled ROM
`platform/playdate/Source/` currently contains `game.min` and
`lunch_time.min` — these are **copyrighted Pokémon Mini cartridges** and
shipping the `.pdx` with them in the bundle is straightforward
infringement. The release `.pdx` must contain freebios only; users place
their own ROM dumps into the Playdate Data folder
(`/Data/com.pokemini.playdate/` on device) post-install.

CMakeLists.txt currently copies whatever's in `Source/` into the bundle
— need a `.gitignore` for `*.min` and a clean `Source/` policy.

### 1b. EEPROM save/load on device
`PokeMini_Playdate.c:390` notes that EEPROM save uses `fopen` (works on
simulator, silent fail on device). `load_rom` already uses `pd->file->*`
APIs correctly — the same pattern needs to wire into PokeMini's
`PokeMini_CustomSaveEEPROM` / `PokeMini_CustomLoadEEPROM` callback hooks.
Path scheme: `/Data/com.pokemini.playdate/eeprom/<rom_basename>.eep`.

Without this, players lose all in-game progress whenever the app exits
— ship-blocker for any RPG-style game.

### 1c. ROM picker (when more than one ROM is present)
Currently hardcoded `load_rom("game.min")`. For a real release:
- Enumerate `*.min` files from the Data directory at startup
  (`pd->file->listfiles`)
- If 0 found: show a friendly "place your ROMs in
  `/Data/com.pokemini.playdate/` and restart" screen with the actual
  device path printed.
- If 1 found: load it directly (current behavior, just dynamic name).
- If 2+ found: simple list-picker UI before booting the emulator.

### 1d. License + attribution
PokéMini is GPLv3. The repo has the upstream license but the Playdate
release should ship with a clearly visible:
- `LICENSE` file in the bundle's `Source/` (or accessible via in-game
  About screen).
- Credits screen listing JustBurn (upstream) + this port author + the
  freeBIOS author.

### 1e. Card art and pdxinfo polish
`Source/pdxinfo` currently has `imagePath=` (empty). Playdate's launcher
shows a generic icon. Need at least:
- A 350×155 launch card image (`launchImage` in pdxinfo).
- A 32×32 menu icon.
- Updated `author=` (currently "JustBurn" — should reflect the port author
  with credit to upstream).
- `description=` that actually describes what it is.

## 2. Important UX (would feel broken without)

### 2a. C button mapping
The Pokémon Mini has A, B, **C**, D-pad, Power, Shake = 9 inputs.
Playdate gives us A, B, D-pad, Crank, accelerometer = 7 inputs total.
Currently C is unmapped (PD_KeysMapping in PokeMini_Playdate.c:76 sets
it to -1). Some games genuinely use C.

Options, in order of preference:
- **Crank-pressed-down** (Playdate's `pd->system->isCrankDocked`): use
  crank rotation for one thing, docked state as a chord. Awkward.
- **A+B chord**: press both simultaneously = C. Easy, but conflicts with
  any game that uses A+B.
- **Move "Shake" off the crank to the accelerometer** (see 2c) and put
  C on crank rotation. Cleanest split — most users never use Shake, and
  crank-rotate-to-C is intuitive.
- **In-app remappable controls** (see 2d). Best long-term answer.

### 2b. Pause on Playdate system menu
When the user presses the Menu button to open the system menu, the
update callback keeps firing and emulation keeps running. Audio also
keeps playing. Should pause cleanly:
- Use `kEventPause` / `kEventResume` events in `eventHandler` to stop
  emulating and silence the audio source while paused.
- Same for `kEventLowPower` (charger unplugged near zero battery).

### 2c. Accelerometer-based shake
Currently crank rotation = Shake. Playdate has a real 3-axis
accelerometer (`pd->system->getAccelerometer`) that's a more natural fit
for the PM Shake input. Detection: rolling magnitude of `dx,dy,dz`
above a threshold = shake event. Cost: enable accelerometer
(`pd->system->setPeripheralsEnabled`), small battery hit.

This frees the crank for the C button per 2a.

### 2d. In-app settings menu
Use Playdate's system menu API (`pd->system->addMenuItem`,
`addOptionsMenuItem`) to expose:
- **Reset game** (PokeMini_Reset)
- **Audio**: on / off (toggle CommandLine.sound, mute the audio source)
- **LCD mode**: smooth (ANALOG, current) / sharp (2SHADES) — see NOTES.md
  for the tradeoff
- **C button**: which input does C map to (see 2a)
- **Save state** / **Load state** slots (see 2e)
- **Quit to ROM picker** (after 1c lands)

### 2e. Save states (in addition to EEPROM)
PokéMini already has full save/load state code (`POKELOADSS_*` /
`POKESAVESS_*` in every component). Wire it up:
- Bind to system menu items: "Save state slot 1/2/3", "Load state slot
  1/2/3".
- Path: `/Data/com.pokemini.playdate/states/<rom_basename>.s1` etc.
- Include a thumbnail in each save (optional; can just be a screenshot).

## 3. Quality polish (release-grade feel)

### 3a. Per-ROM data isolation
Currently EEPROM and save states (when added) live in flat directories.
Better: `/Data/.../<rom_basename>/eeprom.eep`, `.../states/`, etc.
Multiple ROMs with the same internal name don't clobber each other.

### 3b. Empty-state UI
First-launch flow when no ROM is present needs to be friendly:
- Detect zero-ROM state, draw a "How to add ROMs" screen with the actual
  device data path (`pd->system->getCurrentTimeMilliseconds` is unrelated
  but `getDeviceUUID` and similar can help resolve the path) and a hint
  about file extension `.min`.
- Don't crash, don't leave a black screen.

### 3c. Background/foreground audio handling
Audio source created in `eventHandler`'s `kEventInit` should be torn
down on `kEventTerminate`. On `kEventPause` mute it; on `kEventResume`
re-enable. Otherwise audio buffer underrun could happen on resume.

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

## 4. Code/build hygiene

### 4a. Remove the lingering test ROM
`Source/game.min` and `Source/lunch_time.min` need to leave the source
tree. Add `Source/*.min` to `.gitignore`. CMakeLists could either skip
copying `*.min` (cleaner) or accept it as a dev-only convenience.

### 4b. README for the platform
`platform/playdate/` has `NOTES.md` (development notes) and `ROADMAP.md`
(this file) but no end-user `README.md`. Needs to cover:
- What this is
- How to install (sideload `.pdx`)
- How to add ROMs (path, file format)
- Controls
- Known issues
- Build instructions for developers

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
- **Closing the heavy-scene 7% emulation deficit** — covered in NOTES.md.
  Not a ship blocker (most players won't notice; the games that hit it
  worst are the most demanding ones, where slight slowdown is plausible
  even on real hardware).

## Suggested order

A reasonable shipping path:
1. **1a, 1d, 4a, 4b** — clean repo, get LICENSE/README right (an afternoon)
2. **1b** — EEPROM persistence (a few hours, biggest player-facing win)
3. **2b, 3c** — pause/resume handling (an hour each)
4. **1c, 3a, 3b** — ROM picker + per-ROM data + empty-state (a day)
5. **2a, 2c, 2d** — controls (C button), accelerometer shake, settings
   menu with the above wired in (a day)
6. **2e, 1e** — save states + card art (half day)
7. **3d, 3e, 3f** — polish pass (as time allows)

That's roughly 3-4 days of focused work to a v1.0 release.
