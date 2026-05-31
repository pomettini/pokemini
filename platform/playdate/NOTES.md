# PokeMini Playdate Port — Development Notes

Living document of bugs found, fixes applied, and gotchas learned while porting
the PokeMini emulator to the Playdate. Update this whenever something
non-obvious is discovered. Newest entries on top.

## DTCM / ITCM negative results, full exploration (2026-05-31)

Inspired by CrankBoy's documented DTCM/ITCM benefits, we did a thorough
exploration of placing PokeMini hot data/code in DTCM. **All approaches
failed on the user's Rev A Playdate.**

### Approaches tried

| Approach | Outcome |
|---|---|
| Stack-relocate PM_RAM (8 KB array on `update()` stack) | Stack overflow at game load |
| Stack-relocate PM_RAM (4 KB) | Stack overflow at game load |
| Static linker placement at `0x20000000` | Crash on boot (firmware uses bottom of DTCM) |
| Static linker placement at `0x20010000` | Crash on boot (inside firmware stack region) |
| Runtime allocator at `frame_ptr - 10 KB` (= `0x200073C0`), 8 KB write at boot | Crash on boot |
| Same address, deferred 8 KB write to first update() | Crash on boot |
| Same address, **256-byte** write to first update() | Crash on boot |
| Same address, **no DTCM write at all** (init only) | **WORKS** |
| Same as above + wrapper function layer registered as callback | **WORKS** |

### Conclusion

**Any write of any size to `0x200073C0` (the DTCM address we compute via
CrankBoy's `frame_ptr - PLAYDATE_STACK_SIZE` formula) crashes the firmware
on this Rev A device.** No write = no crash. The crash isn't about the
data we write, the timing, or the size — it's about that specific
address range being firmware-active on Rev A.

### Why CrankBoy works and we don't

CrankBoy's own source comments acknowledge: *"0x2710 is currently used to
avoid occasional DTCM crashes on RevA."* They've empirically tuned for a
specific stack depth at their `dtcm_set_mempool` call. Their `main.c`
does many things between firmware boot and the `dtcm_set_mempool` call
(`init_user_stack`, `srand`, `exec_array` for init arrays, `pd_revcheck`,
user stack test) which all push the stack deeper than ours, putting their
mempool at a DIFFERENT (and apparently safer) address.

Replicating this exactly would require matching their startup flow,
which is invasive and provides no signal that we'd land on a safe
address either.

### Mechanism understood, not implementable here

We learned:
- The Playdate firmware stack lives in DTCM (top, growing down)
- Available "free" DTCM is below the stack region
- The SDK reserves 60 KB stack but actual usage is far less; the gap is
  CrankBoy's playground
- On Rev A the firmware actively uses specific DTCM addresses outside
  the stack region in ways the SDK doesn't document

What we CAN'T do without firmware-side debugging:
- Find which DTCM addresses are safe to use on Rev A
- Establish a stable way to allocate DTCM without rolling the
  device-specific dice

### Files left in tree (gated off by default)

- `PokeMini_DTCM.c/.h` — runtime mempool allocator, CrankBoy pattern
- `PokeMini_UserStack.c/.h` — separate stack switching via inline asm
- `PokeMini_ITCMCallbacks.c` — relocatable memory callbacks
- `POKEMINI_PM_RAM_DTCM`, `POKEMINI_ITCM_CALLBACKS` CMake flags

All scaffolding is gated behind off-by-default CMake options. The
shipping build is unaffected. Re-enable for future revisits if Rev B
testing happens, or if someone with firmware-debug access wants to
probe safe addresses.

### Measured cost of the scaffolding (even with no DTCM writes)

Race Mini full lap, `POKEMINI_PM_RAM_DTCM=1` build with the trivial
wrapper and NO DTCM activity (only PM_RAM-as-pointer ABI):

- Steady-state median: **~18.5-18.7 fps**
- Baseline (regular build): **~19.1 fps**
- **Net: ~0.5 fps regression**

Cause: `PM_RAM` as a `uint8_t *` (vs `uint8_t[]`) adds one LDR
instruction per PM_RAM access — the compiler must load the pointer
before computing the indexed address. Race Mini does millions of
PM_RAM accesses per second; cumulative cost is measurable.

This matches Beetle VB / Red Viper's prior findings on the same
pattern. The scaffolding has a baseline tax that the DTCM benefit
would need to overcome to break even. Since DTCM writes crash on
Rev A, we never get to test the upside.

### Phase data still applies

The original phase split for Race Mini holds: 84% CPU dispatch, 11% PRC
+ Timers sync, ~1% render. Even if DTCM had worked, it would only have
helped the memory-load portion of dispatch, which is a small fraction
of the 84%. The performance ceiling for PokeMini on M7 Rev A appears
to be the **interpreter dispatch itself**, not memory latency. JIT is
the only remaining structural lever and it's explicitly out of scope.

## Race Mini phase profile + DTCM negative result (2026-05-31)

Pokemon Race Mini steady-state ~19 fps (vs Togepi 27 fps) is the heaviest
case on the platform. PD_PERF_DIAG measurements during a steady lap (after
adjusting for the diag instrumentation's own ~75% overhead):

| Phase | % of update |
|---|---|
| MinxCPU_Exec dispatch loop | **84%** |
| └ main XX opcodes | 65% of CPU |
| └ CE prefix dispatch | 25% of CPU |
| └ CF prefix dispatch | 10% of CPU |
| MinxTimers_Sync | 5% |
| MinxPRC_Sync | 6.5% |
| Emu loop overhead | 3.4% |
| render_screen + input + misc | <1% |

PD_OPCODE_DIAG steady-state distribution: **E7 (22.7%) + 95 (20.9%)** dominate
main-XX dispatch — together 43% of all CPU work. CE prefix internals are
flat (40, 28, D0 each 10-17% of CE). CF is dominated by B1/B5 (60% of CF
combined) which already have stack-local paths per NOTES.

### Why DTCM is the wrong lever here

PM_RAM is 4 KB + 256 B and very hot, so on paper relocating it to DTCM (per
Beetle VB's ITCM-hot-callback-copy pattern) looks promising. **But the phase
data says it isn't.** 84% of frame time is *opcode dispatch* — the switch
table, opcode bodies, per-call overhead — not data loads. DTCM accelerates
memory access, not interpreter throughput. PM_RAM is small enough to likely
be M7-D-cache-resident already.

This matches Red Viper's negative DTCM result (interpreter throughput
limited, working set fits in cache) more than Beetle VB's modest positive
(callback-funnelled core, hot data outside cache).

### DTCM attempt + stack-budget finding (negative result)

Tried the stack-relocation pattern anyway (per user request):
`PM_RAM[8192]` copied to `uint8_t pm_ram_dtcm[N]` on the stack at update
entry, `PM_RAM` global pointer redirected, copied back at end. Changed
`PM_RAM` from array to pointer ABI behind `-DPOKEMINI_PM_RAM_DTCM=1`.

- **8 KB buffer (full PM_RAM size): stack overflow on game load.**
- **4352 B buffer (only the active 0x1000-0x20FF range): also stack
  overflow.**

The Playdate stack budget for PokeMini's call chain is **less than 4 KB**.
This is unexpectedly tight — Beetle VB (under FreeRTOS) fit a 1 KB buffer,
2 KB overflowed. PokeMini is bare-metal so we expected more headroom; we
have less. Hypothesis: PokeMini_EmulateFrame's call chain
(`EmulateFrame → MinxCPU_Exec → opcode handler → Fetch8 → MinxCPU_OnRead`,
with CE/CF prefix dispatchers adding 2-3 frames per opcode) is deeper than
Beetle VB's V810 interpreter, eating most of the stack budget.

### Conclusion

The DTCM-for-PM_RAM lever is **not viable** without moving to a fixed-
address DTCM region via linker script, which would risk colliding with the
firmware's stack (Beetle VB measured stack reaching down to `0x20000060`).
The infrastructure is reverted. Confirmed for the record: even if it had
fit, the phase data predicted it wouldn't have helped meaningfully.

**What would help Race Mini** is opcode-dispatch reduction, not memory-
latency. E7 (22.7%) is the largest unmet target — previously tried via
local-read inlining and reverted due to dispatcher I-cache pressure. Next
experiment: E7 retry via a different structural choice (case reorder /
early-exit at top of switch) that doesn't grow the per-case bodies.


## synccycles ceiling: PRCCnt window skipping (2026-05-15)

Older notes treat `synccycles = 512` as an empirical hardware ceiling
("Don't push this past 512"). That framing is wrong — it's a fixable
**software bug** in `MinxPRC_Sync`'s state machine, with a mathematically
derivable safe upper bound around 854.

### Mechanism

`MinxPRC_Sync` in `source/MinxPRC.c:121`:

```c
MinxPRC.PRCCnt += MINX_PRCTIMERINC * PokeHWCycles;
```

Disassembly of `MinxPRC_Sync` in a release build (`objdump`,
`0x490a: movw r0, #19622`) confirms `MINX_PRCTIMERINC = 0x4CA6 = 19622`.

The state machine downstream of that increment fires triggers on
discrete `PRCCnt & 0xFF000000` windows:

- BG&SPR render trigger: `PRCCnt & 0xFF000000 == 0x18000000` (16M wide)
- Copy-to-LCD trigger:   `PRCCnt & 0xFF000000 == 0x39000000` (16M wide)
- End-of-frame reset:    `PRCCnt >= 0x42000000`

Each "window" is `0x01000000 = 16,777,216` PRCCnt units = `16777216 / 19622
≈ 854 CPU cycles`. If a single `MinxPRC_Sync` call advances PRCCnt by more
than 854 cycles' worth, PRCCnt can jump entirely past a window and the
matching `else if` branch never fires that frame. `MinxPRC_Render` /
`MinxPRC_CopyToLCD` is silently skipped. `LCDPixelsA` stays blank → white
PM screen. CPU and audio threads keep running because they aren't
gated on the PRC state. Exactly the documented symptom.

### Fix

`CommandLine.synccycles` was 512; now **800** (in `kEventInit`). 800
leaves margin for the `while (PokeHWCycles < synccylc)` overshoot — the
loop exits when PokeHWCycles first exceeds synccylc, so the actual count
can be `synccycles + opcode_cycles - 1`, max ~10 cycles of slack.

Expected throughput delta on heavy ROMs: PRC + Timers sync phases are
~23% of heavy-scene time and Sync is called 800/512 = 1.56× less often,
so sync overhead drops ~36% × 23% ≈ **+5-8% projected throughput**.
Pending device confirmation.

### Going higher

Above ~854 you'd need to chunk the increment inside `MinxPRC_Sync`
(split a large `PokeHWCycles` into ≤854-cycle slices and re-check
triggers between each), or rework the trigger windows to be edge-detect
instead of range-check. Probably not worth it given how much the 800
bump already buys.

## Keepalive mechanism confirmed (2026-05-31)

A/B tested K_BUSY (2000-iter `volatile` nop busy-loop, no syscalls)
against the shipping 4× `getElapsedTime()` recipe on the same device,
same Togepi JP scene, smooth+3x. Both at `synccycles = 800`. Same log
block once-per-second in both.

| Build | Steady median | Best window | Worst dip |
|---|---|---|---|
| Default (4× getElapsedTime) | ~27.0 fps | 28.7 | 19.4 |
| K-busy (NOP loop) | ~27.0 fps | 28.9 | 18.0 |

**Statistical parity.** The dip pattern differs slightly between runs
(sprite-heavy moments hit at different frame offsets) but the
steady-state range is identical.

### Conclusion: M7 clock-state idle scaling

The keepalive's per-update component is about the M7 core looking
busy to the firmware. Any sustained per-update work prevents an
idle-downclock. The specific syscall family is irrelevant — `volatile`
NOPs work the same as `getElapsedTime`. NOTES's earlier "we don't
fully understand why" can be retired: the mechanism is M7 DVFS-like
behavior.

### What's still load-bearing

The **once-per-second format-rich `logToConsole`** is still load-
bearing through a separate mechanism (likely a USB serial /
scheduler tick). Constant-string logs don't work; the `%f`/`%.1f`
formatting is what counts. Removing it caps fps around 12-14 even
with the busy-loop running. Keep it.

### What shipped

`PokeMini_Playdate.c` keepalive block now uses the busy-loop directly
(no `KEEPALIVE_VARIANT` CMake scaffold; that was diagnostic and is
removed). Comment explains the two-mechanism reality:
1. busy-loop → prevents M7 downclock
2. once/sec format-rich log → keeps USB/scheduler tick warm

### What's *not* recovered by this

Identifying the mechanism didn't recover the ~1.4-1.5 fps deficit
between this build and the no-EEPROM baseline. Both keepalive variants
land in the same fps range, so the deficit is somewhere else — D-cache
shifts from the .bss layout changes documented in "Baseline vs EEPROM
build measurements" below.

## EEPROM via callback swap + linker-pinned update (2026-05-15)

Architectural cleanup of the EEPROM commit. The previous "Branch predictor
slot engineering" attempt (below) kept the `if (eeprom_load_pending)` check
inside `update()`, which baked 14 bytes of new code into the per-frame hot
path and made `update()`'s shape differ from the no-EEPROM baseline. This
section is the cleanup; the 2 fps gap discussion follows.

The new design:

1. **No per-frame check inside `update()`.** `update()` is now byte-for-byte
   identical to the no-EEPROM baseline. The flag (`eeprom_load_pending`) is
   gone; so is the `if/cbz/bl` block at the top of the EmulateFrame loop.
2. **One-shot callback swap.** `start_emulation_with_rom` calls
   `pd->system->setUpdateCallback(update_first_after_rom, pd)` when the
   loaded ROM has an `.eep` file. `update_first_after_rom` does the file
   read + clock sync, then `setUpdateCallback(update, pd)` swaps the runtime
   back to the steady-state `update()` for every subsequent frame. The
   callback swap costs zero per-frame overhead — the Playdate runtime stores
   one function pointer and calls it.
3. **Linker pins `update` at mod 0x400 = 0x034.** Even with `update()` at
   baseline byte-shape, its absolute address depends on everything before it
   in the binary. The linker script in `link_map.ld` now uses
   `EXCLUDE_FILE(*PokeMini_Playdate.c.obj) .text.*` to land all
   non-Playdate-port code first, then `. = ALIGN(0x400) + 0x34;` pins
   `*(.text.update)`, then the remaining PokeMini_Playdate.c sections
   (cold paths) fill in afterward. Result: `update` lands at `0x17434`
   (mod 0x400 = 0x034), the `blt.w` loop entry at slot **0x226** (baseline
   match), and the `BL PokeMini_EmulateFrame` at slot **0x1EA** (baseline
   match). Padding cost: ~688 bytes (`*fill*` at 0x17184).

This eliminates the BTB-aliasing gamble entirely and keeps the EmulateFrame
loop in the same predictor slots as the keeper baseline.

### What about `PokeMini_EmulateFrame`?

Still pinned at `0x4c20` (I-cache set 1) via the existing
`.text.emulate_frame` section + `ALIGN(0x400) + 0x20` placement. That fix
(NOTES "EEPROM save/load and I-cache aliasing" below) is independent — it
addresses cache-set collision with `MinxTimers_Sync` at set 19, not BTB.

### Memory layout invariants

After any future code change to `PokeMini_Playdate.c`:
- Verify `update`'s start address still lands at mod 0x400 = 0x034 in
  `game.map`. If the EXCLUDE_FILE list grows, padding may need adjustment.
- Verify `PokeMini_EmulateFrame` is still at `0x4c20` (or any address with
  bits[9:5] = 1, i.e., set 1).

### Baseline vs EEPROM build measurements (2026-05-15)

Controlled on-device comparison, Togepi no Daibouken (Japan), smooth+3x.

**Baseline (HEAD~1, pre-EEPROM commit), early game (no save):**
- Steady-state windows: clusters at 28.5 fps (six consecutive: 28.5/28.5/28.5/28.5/28.5/28.5), 28.2 fps (six consecutive)
- Peak window: 29.6 fps
- Dips: 14.6 (intro), 19.8, 22.4, 23.8-24.0 fps — content-driven
- **Eyeballed steady median: ~28.3 fps**

**Baseline (HEAD~1), title screen:**
- **Rock-solid 28.8 fps constant** (1040-1042ms windows, zero variance)

**Path 1 build (current, with EEPROM machinery, .eep loaded), early game:**
- Steady-state windows: 25-27 fps clusters (1110-1140 ms windows)
- Peak window: 29.6 fps
- Dips: 13-22 fps with similar frequency
- **Eyeballed steady median: ~26 fps**

**Path 1 build, title screen:**
- **27.4 fps constant** (1093-1096 ms windows, zero variance)

Conclusion: the gap is **real** and shows up even on the title screen,
where content is identical between baseline and Path 1. The per-frame
delta is `1093 - 1042 = 51 ms per 30 frames = 1.7 ms per frame` of
pure code overhead. This eliminates "game state matters" — the cost is
introduced by linking the EEPROM machinery itself, not by Togepi reading
saved data differently.

The cost cannot be from `update()` directly (byte-identical to baseline)
or from `PokeMini_EmulateFrame` (same code, same I-cache set, only its
absolute address moved). It must be from something pulled in by the
EEPROM commit that shifts cache behavior on the hot path.

### The 2 fps gap is real but architecture-independent (2026-05-15)

After the callback swap + linker pin landed, on-device testing (Togepi JP,
smooth+3x, with .eep loaded) still averaged ~25-26 fps vs the no-EEPROM
baseline's reported 28 fps. Several experiments ruled out architectural
causes:

| Hypothesis | Test | Result |
|---|---|---|
| `if (eeprom_load_pending)` cost | Path 1: removed via callback swap | No change |
| BTB slot mismatch (NOTES theory below) | Linker pin: slots now match baseline exactly | No change |
| `.text.emulate_frame` set-1 pinning | Removed: EmulateFrame at set 16 (natural) | Lost ~2 fps |
| `pd` non-static vs static codegen | Reverted to static via get_pd() shim | Identical `update()` codegen, no fps change |

The set-1 pinning IS load-bearing (lose it → lose 2 fps); everything else
is a wash. Sample windows hit **28.5, 29.6, 28.1 fps** consistently, so the
peak performance is at baseline. The average is dragged down by dips that
appear at regular intervals (1.5-2.5 second windows of 14-22 fps) regardless
of build configuration.

Working hypothesis for the residual gap:
- **Game state matters.** With an .eep file loaded, the player is mid-game
  with more entities, sound events, and active state. The baseline 28 fps
  number was likely measured with a fresh ROM (no save) on early game.
  Apples-to-apples comparison requires running baseline (HEAD~1) on the
  same save file in the same scene.
- The dips are content-driven (sprite-heavy scenes, sound interrupts), not
  code-driven.

### Conclusion

Path 1 (callback swap + linker pin) is the cleanest the architecture can be:
no per-frame check, `update()` byte-equivalent to baseline, BTB slots match
baseline, EmulateFrame at I-cache set 1. If further fps recovery is needed,
the next place to look is content-driven cost (audio thread, sprite render,
specific PM opcodes), not code layout.

## Branch predictor slot engineering for EEPROM build (2026-05-15, superseded)

Earlier attempt at the EEPROM 2-fps gap. Superseded by "EEPROM via callback
swap + linker-pinned update" above — kept here for the analytical context
on BTB aliasing on Cortex-M7 Rev A, which is still relevant for any future
hot-path code that intersects update()'s alignment.

The I-cache set fix (pinning `PokeMini_EmulateFrame` at set 1 via
`.text.emulate_frame`) recovered 3 fps (22→25) but left a persistent 2-3 fps
gap vs baseline (28 fps). Analysis of the Playdate forum thread
("Dirty optimization secrets") and the NOTES on Codex's 0x140 sweep revealed
the missing piece: **branch predictor table aliasing**.

The BTB on Cortex-M7 Rev A holds ~1024 entries indexed by instruction address
mod 0x400. Two `BL` instructions at the same mod-0x400 slot compete for the
same entry; if one of them aliases a hot `MinxCPU_Exec` internal branch (the
256-case switch fires many times per PM cycle), every hit causes a
misprediction and a pipeline flush.

**Baseline:** `update` starts at 0x6034. The `BL PokeMini_EmulateFrame`
inside the frame loop is at approximately 0x6034 + 0x5B4 = 0x65E8. Slot:
0x65E8 mod 0x400 = **0x1E8**.

**EEPROM build (broken):** `update` shifted to 0x63d4 (total shift 0x3A0 =
code growth 0x80 + EmulateFrame-section libgcc shift 0x320). BL now at
~0x63d4 + 0x5B4 = 0x698**8**. Slot: 0x6988 mod 0x400 = **0x188**. Different
bucket — hit a hot MinxCPU_Exec opcode in the BTB, constant mispredictions.

**Fix:** engineer total shift = exactly **0x400** (one BTB period), restoring
`update` to mod 0x400 = 0x034 → BL slot 0x1E8 (baseline). Do this by:

1. Keep `*(.text.emulate_frame)` pinned with `. = ALIGN(0x400) + 0x20`
   → EmulateFrame at set 1, libgcc shift = 0x320.
2. Total code growth before `update` must = 0x400 − 0x320 = **0x0E0** = 224 bytes.
   Currently 0x80 (start_emulation_with_rom). Gap = 0x60 = 96 bytes.
3. Extract the `if (eeprom_load_pending)` block from `update()` into a
   `__attribute__((noinline))` helper defined **before** `update()` in the
   source file. This adds ~0x60 bytes of code before `update` in the binary
   (via its own `.text.*` section), closing the gap, while keeping `update()`
   nearly at baseline size.

Result: total shift = 0x80 + 0x60 + 0x320 = 0x400. `update` at 0x6034 +
0x400 = 0x6434, same BTB slot as baseline. EmulateFrame stays at set 1.

### Why the sweep approach was necessary

No analytical shortcut: the good BTB slot (0x1E8) was found by inspecting
the baseline binary. The engineering approach above deliberately targets it,
analogous to Codex's 0x40/0x80/0x140 offset sweep for MinxCPU_Exec. The
forum confirms: identical code at a bad BTB slot can cost 20-50% throughput.

### Remaining 2 fps gap (unresolved)

Best achieved: **~26 fps** (vs 28 fps baseline). The residual comes from a
12-byte shortfall: total code-growth-before-update = 0xD4, target = 0xE0.
The gap cannot be closed by changing `link_map.ld` padding alone — the
linker's 32-byte section alignment fills absorb small padding changes without
propagating them to `update`'s address.

Attempted adjustments (sweeping padding from +0x14 to +0x2C): none changed
`update`'s address; EmulateFrame shifted sets but `update` stayed at 0x6428.

The 12-byte shortfall also shifts the `blt.w` (loop condition for the
EmulateFrame loop) from baseline slot 0x226 to 0x21A. This mismatch is the
likely cause of the residual ~2 fps gap. It cannot be fixed without adding
exactly 12 bytes of code to PokeMini_Playdate.c before `update`, which
would require either a semantically meaningful 12-byte operation or a
deliberate assembly NOP pad — both undesirable in production code.

**Shipped state**: EmulateFrame at set 1 (0x4c20), `update` at 0x6428,
BL-to-EmulateFrame at BTB slot 0x1E8 (exact baseline match). The `blt.w`
is 12 bytes off baseline slot; the cost is ~2 fps average.

### Memory layout invariant to maintain

After any future code change to `PokeMini_Playdate.c`:
- Recheck `update`'s start address in the map (`game.map`).
- Target: **0x6034 + N×0x400** for some integer N (mod 0x400 = 0x034).
- Also check `PokeMini_EmulateFrame` stays at I-cache **set 1** (bits[9:5]
  of address = 1). If it drifts into set 8 or 19, fix the EmulateFrame
  section padding in `link_map.ld`.
- If `update` drifts, adjust by adding/removing code before `update` in
  `PokeMini_Playdate.c` or by changing the EmulateFrame padding.

## EEPROM save/load and I-cache aliasing (2026-05-14)

Adding EEPROM helpers to `PokeMini_Playdate.c` caused a persistent ~6 fps
regression (28 → 22 fps, smooth+3x) despite the helpers being startup-only
(never called per-frame). Root cause: the 4× `pd->file->mkdir` calls in
`ensure_eep_dir()` and the 8 KB EEPROM file read in `pd_load_eeprom()` were
initially placed in `kEventInit` and `start_emulation_with_rom` respectively;
those were fixed (deferred load, lazy dirs). But performance still did not
recover.

The compiled `update()` was byte-for-byte identical in both builds (confirmed
via `arm-none-eabi-objdump` diff). The regression was entirely from linked
binary layout:

- Placing EEPROM helpers in `PokeMini_Playdate.c` (the first file in
  `GAME_SOURCES`) grew the file by ~500 bytes, shifting all subsequent
  `.text.*` sections by the same amount.
- `PokeMini_EmulateFrame` (in `Hardware.c`) was pushed from **0x7c20**
  (I-cache set 1, spans sets 1-8) to **0x7e40** (set 18, spans sets 18-25).
- `MinxTimers_Sync` is permanently at **0x4670** (I-cache set 19).
- At the shifted address, `PokeMini_EmulateFrame`'s second cache line
  (0x7e60-0x7e7f) lands in **set 19** — the same set as `MinxTimers_Sync`.
  Every `MinxTimers_Sync` call evicts EmulateFrame's set-19 line and vice
  versa, creating a ping-pong cache conflict on every PM frame. This costs
  ~6 fps steady-state.

**Fix:** Move the EEPROM helper functions to a separate source file
(`PokeMini_Playdate_EEPROM.c`) listed **after `Hardware.c`** in `GAME_SOURCES`.
This places the helpers after `PokeMini_EmulateFrame` in the binary, so they
do not shift it. `PokeMini_EmulateFrame` stays at 0x7c20 (sets 1-8), clear of
`MinxTimers_Sync` (set 19).

### Why PokeMini_EmulateFrame's address matters

The Cortex-M7 has a 4 KB, 4-way set-associative I-cache with 32-byte lines
→ 128 lines, 32 sets. Set index = bits[9:5] of the instruction address.

`MinxTimers_Sync` is always at 0x4670 (set 19) because it is in `.text.hot`
and the hot section is fixed by the custom `link_map.ld`. If any frequently-
executed function occupies set 19 at the same time, they evict each other on
every call.

`PokeMini_EmulateFrame` (248 bytes = 8 cache lines) occupies sets S through
S+7 from its start address. Safe positions:
- Sets 1-8: baseline (start ≤ set 11). Set 19 not in range. ✓
- Sets 18-25: the bad zone from the EEPROM build. Set 19 IS in range. ✗
- Sets 20+: also safe (19 not in range). ✓

### Also: PokeMini_LoadEEPROMFile was GC'd in the baseline

The linker (`--gc-sections`) eliminated `PokeMini_LoadEEPROMFile` (in
`source/PokeMini.c`) from the baseline build because nothing called it.
Initially the EEPROM build pulled it in via `PokeMini_LoadEEPROMFile()`,
adding ~0x7c bytes before `PokeMini_EmulateFrame`. Fixed by calling
`pd_load_eeprom()` directly in the deferred block, keeping the function GC'd.

### Deferred EEPROM load (kept out of kEventInit / start_emulation_with_rom)

All flash I/O is deferred to the first `update()` call after ROM selection:
1. `start_emulation_with_rom` sets `eeprom_load_pending = 1`.
2. First `update()` in emulator mode: keepalive `getElapsedTime` calls fire
   first, then EEPROM is loaded and clock is synced, then emulation begins.
This pattern avoids file I/O during the Playdate firmware's startup window
(which would suppress the keepalive's +3 fps effect) without losing game data.

## Build commands

### Canonical device performance build

Use this for normal on-device performance testing. This is the configuration
Codex has been using for the keeper builds:

```
cmake -S . -B build-device \
    -DTOOLCHAIN=armgcc \
    -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake \
    -DPD_OPCODE_DIAG_BUILD=OFF \
    -DPD_PERF_DIAG_BUILD=OFF
cmake --build build-device --clean-first
```

Expected package contents:

```
PokeMini.pdx/LICENSE
PokeMini.pdx/pdxinfo
PokeMini.pdx/pdex.bin
```

If `PokeMini.pdx/pdex.dylib` is present, that is the simulator binary and the
package is not the clean device-only test artifact.

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

Diagnostic variants are controlled by CMake cache flags, then rebuilt with
the same device build command:

```
cmake -S . -B build-device -DPD_OPCODE_DIAG_BUILD=ON
cmake --build build-device
```

Turn it back off before making performance `.pdx` files:

```
cmake -S . -B build-device -DPD_OPCODE_DIAG_BUILD=OFF
cmake --build build-device
```

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
for expanded Playdate framebuffer bytes. The init function `init_expand_lut()`
was correctly written but never called from `eventHandler`. Since the table sat
in BSS, every entry was `{0, 0, 0}` — so every framebuffer byte we wrote was 0,
regardless of the source pixels. 0 in the Playdate 1-bit framebuffer means
"black".

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
- **EEPROM load order matters.** Upstream `PokeMini_LoadROM()` formats and
  loads EEPROM before `PokeMini_Reset(0)`, then reset applies CPU/BIOS/RTC
  setup. The Playdate custom ROM loader should keep that order. Loading EEPROM
  after reset can subtly change startup state and makes save-enabled testing
  incomparable with the old no-EEPROM path. Also don't mark EEPROM dirty just
  because host RTC was injected; upstream `PokeMini_SyncHostTime()` writes the
  timestamp directly without forcing an EEPROM save.
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

## Performance: opcode compaction experiments (2026-05-11)

Goal: reduce Playdate device CPU cost on heavy ROMs by making the hottest
opcode dispatch paths fit the Cortex-M7 cache better, without breaking
emulation correctness. The working method is now:

1. Pick a hot opcode family from real device logs.
2. Build a host-side equivalence checker for that exact family.
3. Add a Playdate-only compact path.
4. Compile out duplicate original cases if the compact path makes them
   unreachable.
5. Rebuild device `.pdx`, compare symbol size, then test on real hardware.

The host checkers live in `platform/playdate/tools/`:

- `minx_alu_decode_check.c` — XX `00-3F`
- `minx_mov_decode_check.c` — XX `40-7F`
- `minx_short_jump_decode_check.c` — XX `E4-E7`
- `minx_ce_abs_mov_check.c` — CE `D0-D7`
- `minx_ce_xshort_a_check.c` — CE `00/08/10/18/20/28/30/38/40`
  (kept as documentation of a failed runtime experiment)

### Current good stack

Keep these enabled for now:

- **Compact XX ALU `00-3F`** (`POKEMINI_COMPACT_XX_ALU`). Correctness
  proved by randomized host check. Alone it did not noticeably improve
  performance, but it is safe and became useful once paired with MOV.
- **Compact XX MOV `40-7F`** (`POKEMINI_COMPACT_XX_MOV`). This was the
  first real cache-footprint win. `MinxCPU_Exec` shrank from about
  `0x16b6` to `0x12c2` once duplicate cases were compiled out. Togepi
  improved from mostly low-20s / mid-20s fps windows to many 26-29 fps
  windows.
- **Compact XX short conditional jumps `E4-E7`**
  (`POKEMINI_COMPACT_XX_SHORT_JUMP`). Size change was tiny
  (`0x12c2 -> 0x12b8`), but the hot branch path mattered. Togepi became
  dramatically smoother, often 28-29 fps; Pinball also improved strongly.
  Tetris was neutral-to-slightly-worse in some runs, so keep watching it.
- **Compact CE absolute MOV `D0-D7`**
  (`POKEMINI_COMPACT_CE_ABS_MOV`). Targets hot `CE D0` / `CE D4` patterns.
  It is safe and gave a modest Tetris recovery after the short-jump change.
  `MinxCPU_ExecCE` sat around `0x1de6` with the first version enabled.
- **CF stack B pair `B1/B5`** (`POKEMINI_COMPACT_CF_STACK_B`). Race Mini
  showed `CF B1` / `CF B5` as the dominant CF opcodes. Special-casing only
  `PUSH B` / `POP B` stack access with a RAM-window direct path was small,
  safe, and modestly improved Race Mini's slow sections. Keep for now.
- **CE absolute `D4` local write + `D0/D4` direct paths**. Race Mini showed
  `CE D0` / `CE D4` hot. A narrow local-write path for `CE D4`
  (`MOV [#nnnn], A`) moved Race steady-state from mostly ~19.2-20.0 fps to
  mostly ~20.2-20.7 fps. Specializing the `D0-D7` compact block so hot
  `D0` and `D4` bypass nested register-selection switches was neutral to
  slightly positive and did not meaningfully grow `MinxCPU_ExecCE`
  (`~0x1e0c`).

### Memory benchmark implications

The user shared a Rev B memory benchmark showing a huge gap between
sequential RAM/DTCM and scattered byte RAM access. Key qualitative takeaway:
**random/scattered byte access to normal RAM is extremely expensive on
Playdate**, and Rev A is reportedly much slower than Rev B. This matches
our device data: Race Mini and other heavy scenes bottleneck on emulated CPU
memory traffic, not rendering.

Implications for future work:

- Prefer direct PM RAM fast paths only where the address class is already
  strongly implied (stack window, absolute RAM writes, proven hot local RAM
  reads). Broad memory-dispatch rewrites can lose by adding branches and
  expanding hot code.
- Stack-specific helpers are promising because PM stack traffic is clustered
  and predictable.
- Full DTCM/ITCM placement may still be the biggest remaining hardware-level
  lever, but it needs careful linker/runtime work. Do not assume ordinary
  heap/static RAM is "fast enough" for scattered byte-heavy emulator paths.
- The benchmark supports optimizing for locality and fewer unpredictable
  branches as much as for raw instruction count.

### Important failed experiments

#### Cold-helper opcode split

Moving rare XX cases to cold helpers shrank `MinxCPU_Exec` under 4 KB, but
device performance collapsed to roughly 15 fps. The call/branch/cache cost
was worse than the original switch layout, and several supposedly "cold"
paths were not cold enough.

Lesson: **do not chase 4 KB at any cost**. A smaller dispatcher can be slower
if it turns fallthrough switch code into calls or poorly-predicted branches.

#### First compact ALU attempt

The first `00-3F` decoded ALU path glitched the screen and never got past
boot. Root cause: `Fetch8()` overwrites `MinxCPU.IR`; the decoded path used
`MinxCPU.IR` after fetching operands, so immediate bytes could be mistaken
for the opcode.

Fix pattern: capture the opcode immediately after the instruction fetch:

```c
MinxCPU.IR = Fetch8();
uint8_t opcode = MinxCPU.IR;
```

Then use `opcode` for all decode decisions. The original `switch(MinxCPU.IR)`
was safe because the switch expression is evaluated once before case bodies
call `Fetch8()`.

#### CE `[X+#ss] -> A` compact family

Tried a compact path for CE `00/08/10/18/20/28/30/38/40`, including hot
`CE 28` and `CE 40`. The host checker passed and `MinxCPU_ExecCE` shrank
(`0x1de6 -> 0x1d14`), but Tetris gameplay dropped badly to around 18-19 fps.
Boot animation, however, became very smooth / steady 30 fps.

Current state: **runtime macro disabled**. The checker remains in `tools/`
for reference.

Likely explanation:

- Boot and gameplay have different opcode mixes. The boot sequence probably
  hits this exact compact family often, so it benefited.
- Tetris gameplay likely has heavy CE traffic outside that family. The new
  early CE gate and second-level operation switch became a tax on the broader
  CE stream.
- Smaller code was not faster here: extra branches, more live temporaries,
  and worse layout likely outweighed the saved cases.

#### XX `95` / `TST [HL], #nn` fast path

Race Mini diagnostics showed steady gameplay dominated by `E7` and `95`, so
a narrow Playdate-only path for `XX 95` was tried after the compact short
jump path. The host checker passed, but `MinxCPU_Exec` grew from about
`0x12b8` to `0x12fa` and one-lap Race Mini gameplay dropped from roughly
`19-20 fps` windows to mostly `18-19 fps`.

Current state: **runtime macro disabled**. The checker remains in `tools/`
for reference.

Likely explanation: the instruction itself is too small to justify adding
another hot-path branch to the top-level dispatcher. `E7` is even hotter than
`95` in Race Mini, and although the `95` check was ordered after `E7`, the
extra code still expanded the hot dispatcher enough to hurt I-cache behavior.

Lesson: **symbol size is not the metric; device logs are**. A compact path
must improve the real phase we care about, not only shrink the function.

#### Shared instruction-fetch helper

Tried replacing `Fetch8()`'s generic `MINXCPU_READ(1, addr)` with a shared
Playdate-only `MinxCPU_FetchRead(addr)` that fast-pathed ROM/BIOS fetches
and fell back for RAM/I/O. The helper was tiny (~0x3c bytes) and looked
attractive because instruction fetch is the hottest memory read in the CPU.

Result: Race Mini regressed badly, with steady gameplay falling to roughly
15 fps. Reverted.

Likely explanation: because the helper was a real call on every opcode and
immediate fetch, call overhead plus I-cache pressure overwhelmed the saved
branches. The lesson is the same as the cold-helper split: **do not move
ultra-hot paths behind calls**, even if the callee is small and the logic
looks simpler.

#### CE `48` / `MOV B, [X+#ss]` local-read path

Tried changing only `CE 48` to use `MinxCPU_CE_LocalRead` after the Race Mini
`CE D4` win. It looked plausible because Race diagnostics showed `CE 48`
inside the hot set, but the change grew `MinxCPU_ExecCE` from about `0x1e0c`
to `0x1e4a` and one-lap Race Mini fell back into mostly `19.x fps` windows.

Current state: **reverted**.

Likely explanation: `CE 48` is not hot enough, or its address pattern is not
RAM-local enough, to pay for expanding the already cache-sensitive CE
dispatcher. The `CE D4` win remains because absolute RAM writes are simpler
and more predictable.

### Current game observations

- **Togepi no Daibouken**: biggest beneficiary. ALU+MOV helped; adding
  short jumps made it feel very smooth, with many 28-29 fps windows.
- **Pokemon Pinball Mini**: strong beneficiary. Baseline sustained gameplay
  was about 20.5-21.4 fps; current good stack is mostly 22.8-23.7 fps with
  early windows near 28-30 fps.
- **Pokemon Tetris / Shock Tetris family**: mixed. ALU+MOV was neutral to
  slightly positive; short jumps may be slightly worse; CE `D0-D7` modestly
  recovered some ground. The failed CE `[X+#ss] -> A` family was clearly bad
  for gameplay despite improving boot.

### Longer opcode-diagnostic runs

These runs use the opcode diagnostic build, so raw fps is lower than normal
device builds and should not be compared directly. The useful signal is the
relative opcode mix.

- **Race Mini, three-lap diagnostic**: sustained gameplay stays heavily
  dominated by `XX E7` / `XX 95`, with `CE` and `CF` as the next important
  buckets. The steady slow sections repeatedly show `CE 40`, `CE 28`,
  `CE D0`, `CE D4`, `CE 8D`, `CE 98`, and `CE 48`; `CF B1` / `CF B5`
  dominate CF, followed by smaller `CF 0B`, `CF E6`, `CF 20`, `CF 19`,
  `CF EC`, and `CF 42` clusters. The current `CE D0/D4` and `CF B1/B5`
  work attacks the most stable cross-window targets.
- **Togepi long diagnostic**: early sections look similar to Race, with
  `XX E7` / `XX 95` far ahead and `CF B1` / `CF B5` very hot. Later heavy
  gameplay shifts more work into `CE` / `CF`: common CE leaders are `CE 28`,
  `CE 40`, `CE D4`, `CE D0`, `CE C5`, `CE D8`, `CE 51`, and `CE 48`;
  common CF leaders after `B1/B5` are `CF 01`, `CF 00`, `CF 20`, `CF 42`,
  `CF C1`, and `CF 60`.
- **Pinball long diagnostic**: confirms the same two-phase shape. Early/menu
  and lighter gameplay is again dominated by `XX E7` / `XX 95`, while active
  ball sections push much more work into `CE` and `CF`. Stable CE leaders are
  `CE 28`, `CE D0`, `CE D4`, `CE 93`, `CE 40`, `CE C5`, `CE D8`, and
  sometimes `CE 51` / `CE 83`. CF is dominated by `CF B1` / `CF B5`, with
  `CF 01`, `CF 20`, `CF 42`, `CF C0`, `CF B4`, `CF B0`, and `CF 60`
  recurring behind them.

Interpretation: the current wins are pointing in the right direction, but the
next candidate should be chosen from opcodes that recur across Race, Togepi,
and Pinball. Avoid retrying broad CE families or `XX 95` unless there is a new
reason; the diagnostics make them tempting, but previous A/B runs showed code
layout and branch pressure can erase the apparent win.

### CF stack A pair experiment - reverted

After the Race/Togepi/Pinball diagnostic pass, the pure register arithmetic
candidates (`CF 01`, `CF 20`, `CF 42`) looked risky: the existing cases are
already tiny, and a new early decode branch would add layout pressure to every
CF prefix. Instead, the next test extends the proven stack-local fast path from
`CF B1/B5` to `CF B0/B4` as well. This targets `PUSH A` / `POP A`, which show
up behind `B1/B5` in Pinball and are especially relevant to other games like
Tetris/Party Mini.

Implementation: `POKEMINI_COMPACT_CF_STACK_B` became
`POKEMINI_COMPACT_CF_STACK_AB`. `B0`, `B1`, `B4`, and `B5` now use the same
Playdate-only direct stack RAM path with fallback. Normal device build size:
`MinxCPU_ExecCF` is about `0x0ea0`, up from the previous `B`-only keeper
around `0x0e68`.

Race Mini caught the regression immediately: despite a slightly shorter menu
segment, the steady one-lap section fell back into roughly `18.4-19.2 fps`
windows instead of the keeper's roughly `20 fps` range. Current state:
**reverted to `CF B1/B5` only**. Likely explanation: `CF B0/B4` were not hot
enough in Race to pay for the extra CF dispatcher growth, even though they
showed up in Pinball/Tetris-like traces.

### CE/CF prefix timing diagnostic

After the `CF B0/B4` failure, the next diagnostic build extends
`PD_PERF_DIAG` with prefix-handler timing. The normal phase line now includes
`ce=` and `cf=` fields:

```text
diag: upd=30 pm=72 total=...us emu=... cpu=... ce=... cf=... tim=... prc=... aud=... input=... render=... misc=...
```

The `ce` and `cf` values are measured around top-level `XX CE` and `XX CF`
calls inside `MinxCPU_Exec()`. They are included inside the broader `cpu=`
time, not additional time. Use this build only to decide whether prefix
handlers are worth optimizing further; the extra `getElapsedTime()` calls add
overhead and make raw fps unsuitable for A/B comparisons.

Race Mini result: once in sustained gameplay, typical 30-update windows were
roughly:

- `total`: `2.68-2.83s`
- `emu`: `2.66-2.81s`
- `cpu`: `2.20-2.35s`
- `ce`: `0.43-0.48s`
- `cf`: `0.22-0.24s`
- `tim`: `0.16-0.17s`
- `prc`: `0.20s`
- `render`: `0.016-0.018s`

Interpretation: `CE`/`CF` are meaningful, but not the whole CPU cost. In this
instrumented build, `CE` is about 20% of CPU time and `CF` about 10%; combined
they are around 30% of CPU time. Absolute values are inflated because the
measurement itself calls the Playdate timer around every prefix. The relative
shape still matters: `CE` is the larger prefix target, while the remaining
non-prefix top-level stream is still the biggest bucket overall. This argues
against more broad CF stack/layout experiments and for either a very narrow CE
single-op test or a return to top-level layout work with careful A/B.

### CE `93` local read-modify-write experiment

Next narrow CE test after the prefix timing diagnostic: `CE 93` / `ROLC [HL]`
used `MinxCPU_CE_LocalRead()` and `MinxCPU_CE_LocalWrite()` behind
`POKEMINI_CE_93_LOCAL_RMW`. This was deliberately a single-opcode experiment:
`CE 93` appears in Race and Pinball diagnostic windows, performs a memory
read-modify-write, and does not need a new top-level dispatcher branch.

Race Mini one-lap result: no correctness issue, but no keeper-level win. The
steady section stayed mostly in the high-18s to high-19s fps band, while
`MinxCPU_ExecCE` grew from about `0x1e0c` to `0x1e60`. Current state:
**reverted**. Lesson: even a local single-op CE RMW fast path can lose if it
adds enough CE dispatcher size; keep future CE tests either smaller or clearly
hotter than `CE 93`.

### Linker hot-dispatcher offset sweep

After the CE single-op tests started losing mostly through code-size/layout
pressure, the next experiment follows the Playdate "performance lottery"
advice more directly: keep code identical and move the hot dispatcher within
the aligned linker page. First test adds `0x80` bytes after the initial
`ALIGN(0x1000)` in `link_map.ld`, before `.text.hot.exec`.

This is intentionally a pure layout A/B. No opcode logic changes, diagnostics
off. Race Mini one-lap is the first test target because it is currently the
most sensitive workload.

Race Mini one-lap result for `0x80`: clear improvement. Compared with the
18:50 keeper/baseline build, the slow steady section moved from mostly
`19.x fps` windows into mostly `20.5-21.6 fps`, with the early/mid sections
also feeling noticeably smoother on device. The only code change was:

```ld
. = ALIGN(0x1000);
. += 0x80;
*(.text.hot.exec)
```

`MinxCPU_Exec` moved from `0x00000000` to `0x00000080`. Function sizes stayed
unchanged (`Exec=0x12b8`, `CE=0x1e0c`, `CF=0x0e68`), and the package remained
device-only with diagnostics off. This strongly supports the Playdate forum
thread's cache-line / branch-predictor "performance lottery" explanation: on
Rev A, identical code at a different address can produce a large emulator
speed delta.

Current state: `0x80` is the provisional best layout offset. Next best move is
to sweep a few nearby offsets without changing any C code, using Race Mini
one-lap as the first-pass judge. Suggested order: `0x40`, `0xC0`, `0x100`,
then optionally `0x20` / `0x60` / `0xA0` if the curve looks noisy.

Sweep build 1 after the `0x80` win: `0x40` offset. This should be compared
against the `0x80` build and the 18:50 keeper/baseline, not against any CE
single-op experiment.

Race Mini one-lap result for `0x40`: also a strong layout win. The steady
section stayed mostly around `20.6-21.6 fps`, similar to `0x80`, and the
early/mid section had several high-20s windows. `0x40` and `0x80` are both
well above the 18:50 keeper/baseline in Race; continue the sweep before
choosing between them.

Race Mini one-lap result for `0xC0`: another strong result, with the later
steady section often at `21.0-21.8 fps` and no obvious downside versus
`0x40`/`0x80`. This suggests the good region may extend across much of
`0x40-0xC0`; test `0x100` next before doing a finer sweep.

Race Mini one-lap result for `0x100`: still strong. Early/mid sections again
hit many high-20s windows, and the later steady section stayed mostly in the
`20.7-21.8 fps` range. This confirms the good layout region extends at least
through `0x100`. Next fine-sweep point: `0xE0`, between the strong `0xC0` and
`0x100` builds.

Race Mini one-lap result for `0xE0`: regression versus the strong
`0xC0`/`0x100` builds. The later steady section fell back to mostly
`19.4-20.4 fps`, despite `0xE0` sitting between two good offsets and using the
same code/sizes (`Exec=0x12b8`, `CE=0x1e0c`, `CF=0x0e68`). Important clue:
the good tested offsets (`0x40`, `0x80`, `0xC0`, `0x100`) are all multiples of
`0x40`, while `0xE0` is `0x20` off that grid. Treat 64-byte placement as a
likely factor and prefer the next `0x40`-multiple (`0x140`) over more half-step
tests for now.

Race Mini / Togepi / Pinball / Sodateyasan result for `0x140`: strong keeper
candidate. Race one-lap returned to the good layout band: early/mid sections
showed many high-20s windows, and the later steady section mostly stayed
around `21.0-22.0 fps`, better than the `0xE0` regression and competitive with
`0xC0`/`0x100`. Togepi looked excellent, with long stretches around
`28-29 fps`. Pinball's 22-second in-game sample stayed mostly in the
`23-24 fps` band after the initial fast section. Sodateyasan, mostly still
images, ran close to full speed (`28-29 fps`) as expected. This strengthens
the hypothesis that `0x40`-multiple placement matters.

Race Mini one-lap result for `0x180`: competitive with `0x140`, but not a
clear replacement. Early/mid windows were very strong, including several
`26-28 fps` samples, and the later heavy section mostly stayed around
`21.0-22.1 fps`. This keeps `0x180` in the good `0x40`-multiple family, but
`0x140` remains at least as convincing because it also had good Togepi,
Pinball, and Sodateyasan coverage.

Race Mini one-lap result for `0x1C0`: good, but still not a clear improvement
over `0x140`. Early/mid windows were strong and the later heavy section mostly
sat around `20.7-21.6 fps`, with occasional `21.6 fps` windows, but it did not
beat the `0x140`/`0x180` family convincingly. Stop the upward sweep here for
now.

Current production keeper: `0x140` offset. It has the best combined evidence:
Race is strong, Togepi is excellent, Pinball is healthy, and the still-image
Sodateyasan sample stays near full speed. Rebuild `0x140` before broader game
testing.

Audio lifecycle note: one `0x140` device build unexpectedly had silent in-game
audio despite the emulator running normally. The audio source used to be created
during app init while the ROM picker was active and `audio_callback` was gated
to silence. To avoid a layout/lifecycle-sensitive silent-source state on device,
start the Playdate `SoundSource` only after a ROM enters `MODE_EMULATOR`, and
remove it when returning to the picker. This keeps the current CPU layout pinned
while making audio startup explicit at the emulation transition.

Follow-up clue: if the first game after app launch had no audio, opening the
system menu, returning to the ROM picker, and starting any game made audio work.
The delayed remove/add hypothesis did **not** fix it: creating the audio source
at ROM start, then recreating it once after three update ticks still behaved the
same as before. Next hypothesis is that the system-menu trip wakes the Playdate
audio output path rather than repairing the source object, so the current test
explicitly calls `pd->sound->setOutputsActive(1, 1)` when starting emulation and
when resuming from the system menu.

Confirmed: `setOutputsActive(1, 1)` fixed first-launch audio on device. Race
Mini one-lap with this audio fix stayed in the good `0x140` family: early/mid
sections still hit high-20s windows, and the heavier steady section mostly sat
around `20.1-21.1 fps`. Treat the audio-output activation as part of the keeper
stack, not as a temporary diagnostic.

### Guidance for next performance work

- Keep the current good stack as the baseline.
- Do not re-enable `POKEMINI_COMPACT_CE_XSHORT_A` without a specific A/B
  reason.
- Prefer tiny, isolated CE/CF changes over broad decoded CE dispatch.
- For Race Mini specifically, the current profitable direction is narrow
  memory-path specialization in already-hot cases (`CF B1/B5`, `CE D4`,
  and `CE D0/D4` direct paths) rather than new top-level decode gates. `CE 48`
  was tried and reverted.
- If optimizing CE again, consider one hot opcode or a very small family at a
  time, with a checker and a rollback plan.
- Re-run at least Togepi, Tetris, and Pinball after any dispatcher-layout
  change. One game is not enough; opcode locality wins are workload-specific.

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

### Where the remaining 7% deficit lived

Heavy-scene saturation on the original baseline was 100% emulator-core-
bound. The "Dispatcher locality pass" below pushed light scenes to
100% real PM and brought the typical heavy-scene number from 93% to
near full speed too. The remaining unaddressed perf gap is on **heavy
ROMs** — Togepi was the canonical example, with no measurable
difference between the Japanese ROM and its English fan
translation (see correction in the next section).

## Dispatcher locality pass (2026-05-09)

### Trigger

User reported Togepi running noticeably slow. Per-second phase
timing showed the core was saturated at ~990 ms/sec emu but only
emulating 41 PM frames/sec (real PM = 72), at ~17 fps display.

(Originally framed as a Japanese-vs-English-Togepi gap based on
casual play feel. A 2026-05-10 measurement of the English fan
translation [`Togepi's Great Adventure (Japan) [T-En by Mr. Blinky &
Snesy v1.0] [n].min`] showed essentially the same fps profile as
the Japanese ROM — the deficit is workload-driven, not language-
driven. The cause and fix below are unchanged.) Inner-loop split
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
Light scenes were already at 100% — they stay there. (Earlier drafts
said "and English ROMs"; that was speculation, since corrected — the
English fan-translated Togepi runs at ~the same fps as the Japanese
version. Heavy is heavy regardless of language.)

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
ROMs (Togepi, JP or English fan-translation) is **~86% real PM
speed**, with light scenes at 100%. This is where things stay until
either:
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

### Computed-goto dispatcher experiment — reverted (2026-05-09)

Active experiment after `MinxCPU_XX.c -O2` regressed. `MinxCPU_XX.c` now uses
shared opcode-body macros so the same bodies compile either as the original
`switch` or as GCC labels-as-values. The Playdate build temporarily defined
`POKEMINI_COMPUTED_GOTO`, enabling the computed-goto path:

```c
static const void *const handlers[256] = { &&op_00, ... };
goto *handlers[MinxCPU.IR];
```

Scope: only the main `MinxCPU_Exec()` dispatcher. Prefix dispatchers
(`MinxCPU_ExecCE`, `MinxCPU_ExecCF`, `MinxCPU_ExecSPCE`, `MinxCPU_ExecSPCF`)
remain switch-based for this first A/B.

Build result: `PokeMini.pdx` rebuilt successfully. Device `.text` is 113206
bytes, about +512 bytes over the current keeper build (`Hardware.c -O2` +
`MinxTimers.c -O3`, 112694 bytes).

Result from the same Japanese Togepi / smooth LCD / first-three-levels route:
average ~24.45 fps across 49 non-zero windows, with 30 windows >=25 fps, 9
windows >=26 fps, and 2 windows >=27 fps. This is below the current keeper
stack (`Hardware.c -O2` + `MinxTimers.c -O3`, ~25.9 fps), so
`POKEMINI_COMPUTED_GOTO` was removed from the Playdate build. The source-level
fallback remains available if we want to revisit threaded dispatch later, but
the active build is back on the plain switch dispatcher.

### Opcode frequency diagnostic build (2026-05-09)

Diagnostic used after broad dispatcher-shape experiments stopped helping.
When `PD_OPCODE_DIAG` is enabled, the Playdate build counts executed CPU
opcodes in five tables:

- `xx`: top-level opcodes from `MinxCPU_Exec()`
- `ce`: sub-opcodes from `MinxCPU_ExecCE()`
- `cf`: sub-opcodes from `MinxCPU_ExecCF()`
- `spce`: nested special `CE` sub-opcodes
- `spcf`: nested special `CF` sub-opcodes

The Playdate update loop logs compact top-8 windows every 150 display updates:

```text
opdiag: window=150 updates
opdiag: xx total=... top=3E:... CE:... ...
opdiag: ce total=... top=...
```

This is intentionally game-agnostic. Run the same route on Togepi first for
continuity, but also sample other representative games before committing
opcode-specific optimizations. The diagnostic build adds per-instruction
counter overhead, so use it for hot-opcode discovery only, not for fps
comparison.

Togepi JP sample through roughly level 6:

- 16 diagnostic windows, ~20.1M top-level opcodes counted.
- `xx:E7` (`JNZ #ss`) was ~7.14M calls, ~35.5% of top-level dispatches.
- `xx:95` (`TST [HL], #nn`) was ~7.09M calls, ~35.2% of top-level
  dispatches.
- `xx:CE` / `ce:*` was ~1.20M calls, ~6.0%.
- `xx:CF` / `cf:*` was ~0.55M calls, ~2.7%.
- Top `CE` sub-opcodes: `28` (`OR A, [X+#ss]`), `D0` (`MOV A, [#nnnn]`),
  `40` (`MOV A, [X+#ss]`), `D4` (`MOV [#nnnn], A`), `C5` (`MOV I, #nn`).
- Top `CF` sub-opcodes: `B1` (`PUSH B`) and `B5` (`POP B`) dominated,
  followed by `01` (`ADD BA, HL`) and `00` (`ADD BA, BA`).

Interpretation: Togepi is spending most of the CPU dispatch stream in tight
`TST [HL], #nn` / `JNZ #ss` polling or bit-test loops. This is useful, but do
not optimize only for this shape yet. Before committing a source-level opcode
fast path, sample at least one puzzle/menu-heavy game and one action/minigame
ROM to see whether `95/E7` remains dominant or whether memory movement /
prefix opcodes take over.

Pokemon Party Mini Europe sample:

- 52 diagnostic windows, ~8.23M top-level opcodes counted.
- Average top-level dispatches per 150-update window: ~158k. This is much
  lower than Togepi's ~1.26M/window, matching the much smoother 30 fps feel.
- `xx:CE` / `ce:*` was ~1.42M calls, ~17.3% of top-level dispatches.
- `xx:CF` / `cf:*` was ~0.73M calls, ~8.8%.
- `xx:E6` (`JZ #ss`) was ~345k calls, ~4.2%; `xx:E7` (`JNZ #ss`) was only
  ~230k calls, ~2.8%.
- The Togepi-dominant `xx:95` (`TST [HL], #nn`) did not appear in the top
  window summaries, so it is not a universal hot spot.
- Top `CE` sub-opcodes: `D0` (`MOV A, [#nnnn]`), `D4` (`MOV [#nnnn], A`),
  `80` (`SAL A`), `C4` (`MOV U, #nn`), `C5` (`MOV I, #nn`), `C6`
  (`MOV XI, #nn`).
- Top `CF` sub-opcodes: `20` (`ADD HL, BA`), `B0`/`B4` (`PUSH A`/`POP A`),
  `B9`/`BD` (`PUSHAX`/`POPAX`), `B2`/`B6` (`PUSH L`/`POP L`).

Interpretation: Party Mini is not branch/test bound like Togepi. It is much
lighter overall and spends a larger share in prefix dispatch, absolute memory
loads/stores, register-bank setup (`MOV U/I/XI, #nn`), ALU shifts, and stack
traffic. A Togepi-specific `95/E7` fused path is risky as a general
optimization. The safer cross-game direction looked like reducing prefix
dispatch/memory access overhead or improving the hot `MinxCPU_OnRead` /
`MinxCPU_OnWrite` paths, then testing on both games.

Pokemon Pinball Mini USA/Europe sample:

- 12 diagnostic windows, ~14.5M top-level opcodes counted.
- Average top-level dispatches per 150-update window: ~1.21M, much closer to
  Togepi than Party Mini.
- `xx:E7` (`JNZ #ss`) was ~4.68M calls, ~32.2%.
- `xx:95` (`TST [HL], #nn`) was ~4.60M calls, ~31.7%.
- `xx:CE` / `ce:*` was ~1.19M calls, ~8.2%.
- `xx:CF` / `cf:*` was ~0.50M calls, ~3.4%.
- Top `CE` sub-opcodes: `D0` (`MOV A, [#nnnn]`), `28`
  (`OR A, [X+#ss]`), `D4` (`MOV [#nnnn], A`), `40`
  (`MOV A, [X+#ss]`), `93` (`ROLC H`), `D8` (`MUL L, A`), `51`
  (`MOV L, [Y+#ss]`).
- Top `CF` sub-opcodes: `B1`/`B5` (`PUSH B`/`POP B`), `01`
  (`ADD BA, HL`), `20` (`ADD HL, BA`), `60` (`ADC BA, #nnnn`), `42`
  (`ADD Y, BA`).

Interpretation: Pinball confirms that `TST [HL], #nn` / `JNZ #ss` is not just
a Togepi pattern; at least two heavier games spend most top-level dispatches
there. However, the later Pinball windows also shift heavily into `CE`/`CF`
prefix traffic, absolute/indexed memory accesses, stack traffic, and 16-bit
math. Current best optimization direction: first try a broadly useful CPU
memory-access fast path, then compare Togepi + Pinball + Party Mini. A fused
`95/E7` loop path may be worth a later A/B only if the general memory work is
insufficient.

Pokemon Shock Tetris Japan sample:

- 17 diagnostic windows, ~18.0M top-level opcodes counted.
- Average top-level dispatches per 150-update window: ~1.06M, lighter than
  Togepi/Pinball but still a heavy game compared with Party Mini.
- `xx:E6` (`JZ #ss`) was ~5.01M calls, ~27.8%.
- `xx:35` (`CMP A, [#nnnn]`) was ~4.58M calls, ~25.5%.
- `xx:CE` / `ce:*` was ~3.61M calls, ~20.1%.
- `xx:CF` / `cf:*` was ~0.31M calls, ~1.7%.
- `xx:E7` (`JNZ #ss`) was only ~0.13%; this is not the Togepi/Pinball
  `TST [HL], #nn` / `JNZ #ss` shape.
- Top `CE` sub-opcodes were overwhelmingly absolute-memory operations:
  `D0` (`MOV A, [#nnnn]`) at ~44.1% of `CE`, and `D4`
  (`MOV [#nnnn], A`) at ~32.5%, followed by `80` (`SAL A`), `90`
  (`ROLC A`), and `BC` (`CMP B, #nn`).
- Top `CF` sub-opcodes: `B4`/`B0` (`POP A`/`PUSH A`) dominated, followed by
  `40`/`41` (`ADD X, BA` / `ADD X, HL`) and `B9`/`BD`
  (`PUSHAX`/`POPAX`).

Interpretation: Shock Tetris rules out optimizing only for `95/E7`.
The common cross-game theme is memory-heavy compare/test/load/store work plus
flag-setting branches. Heavy games can be `TST [HL]` + `JNZ`, `CMP A,[#nnnn]`
+ `JZ`, or prefix-heavy absolute/indexed loads and stores. This strengthened
the case for a general CPU memory-access fast path before any fused opcode
special case.

### CPU fast memory access experiment - reverted (2026-05-10)

Test after the opcode-frequency diagnostic showed that the heavy games are not
all dominated by the same opcode pair. Instead of fusing one
Togepi/Pinball-style loop, the Playdate build temporarily defined
`POKEMINI_CPU_FASTMEM` with `PD_OPCODE_DIAG` disabled.

Implementation:

- `MinxCPU_FastRead()` and `MinxCPU_FastWrite()` live in `source/Hardware.c`
  and keep the same coarse memory behavior as the existing performance path:
  BIOS below `0x1000`, RAM/framebuffer at `0x1000..0x1FFF`, I/O at
  `0x2000..0x20FF`, ROM at `0x2100+`.
- `source/MinxCPU.h` routes `Fetch8()`, `ReadMem16()`, `WriteMem16()`,
  `PUSH()`, and `POP()` through `MINXCPU_READ` / `MINXCPU_WRITE`.
- The CPU dispatch translation units locally alias `MinxCPU_OnRead` /
  `MinxCPU_OnWrite` to the fast helpers when `POKEMINI_CPU_FASTMEM` is
  defined, so ordinary opcode memory operands get the same fast path without
  rewriting individual opcodes.

First implementation note: an inline-header version made the device binary too
large (`.text` around 137 KB), which is bad for the Playdate M7 Rev A I-cache.
The current function-helper version keeps code size close to the keeper stack:

```text
   text    data     bss     dec     hex filename
 112886    3712   19516  136114   213b2 build-device/PokeMini.elf
```

That was only +192 bytes over the previous keeper build (`112694` bytes for
`MinxTimers.c -O3` + `Hardware.c -O2`), but the device result on Togepi JP was
a clear regression: most steady windows landed around 20-23 fps, with no 25+
fps steady region. Representative sample:

```text
perf: total=120 dt=1363ms (rate=22.0fps)
perf: total=300 dt=1733ms (rate=17.3fps)
perf: total=600 dt=1339ms (rate=22.4fps)
perf: total=900 dt=1743ms (rate=17.2fps)
perf: total=1200 dt=1490ms (rate=20.1fps)
perf: total=1500 dt=1323ms (rate=22.7fps)
```

Conclusion: do not keep this in the active build. The extra function-call hop
for every memory operand appears to cost more than the simpler branch tree
saves. `POKEMINI_CPU_FASTMEM` was removed from the Playdate compile
definitions, leaving the helper code dormant behind its guard in case we want
to revisit a narrower, opcode-specific direct memory path later.

Post-revert sanity check on Togepi JP returned to the keeper range:
~25.9 fps average across 52 non-zero 30-update windows, with 38 windows
>=25 fps, 36 windows >=26 fps, and 27 windows >=27 fps. After the boot/loading
outliers, the run spends long stretches around 27.2-27.8 fps again, confirming
the active build is back to the pre-fastmem performance profile.

### Top-level local read experiment - active test build (2026-05-10)

Next A/B after broad `POKEMINI_CPU_FASTMEM` regressed. Instead of routing every
CPU memory operand through a new helper, `source/MinxCPU_XX.c` now has a small
local inline read helper that directly handles BIOS/RAM/ROM and falls back to
`MinxCPU_OnRead()` only for I/O. It is used only by two read-only top-level
opcodes:

- `0x35` / `CMP A, [#nnnn]`, hot in Shock Tetris.
- `0x95` / `TST [HL], #nn`, hot in Togepi and Pinball.

Rationale: this tests whether direct memory reads help the hottest read-only
opcode bodies without adding an extra function-call hop to every memory
operand, and without touching writes/framebuffer behavior. Test Togepi first
for continuity, then Shock Tetris to see whether the `0x35` path helps its
different hot-loop shape.

Build result: `PokeMini.pdx` rebuilt successfully. Device `.text` is 112822
bytes, +128 bytes over the keeper build (`112694`). This is small enough to
test on device.

Togepi JP result: essentially neutral to slightly positive versus the keeper.
Across 53 non-zero 30-update windows, average was ~25.96 fps, with 40 windows
>=25 fps, 35 windows >=26 fps, 26 windows >=27 fps, and 2 windows >=28 fps.
This is not a clear regression like broad fastmem. Keep the test active and
sample Shock Tetris next, because that game specifically stresses the `0x35`
`CMP A, [#nnnn]` path.

Shock Tetris JP local-read result: across 87 non-zero 30-update windows,
average was ~23.36 fps, with 80 windows >=22 fps, 21 windows >=23 fps,
16 windows >=24 fps, 15 windows >=25 fps, and 11 windows >=27 fps. After the
early transitions, the long steady section averaged ~22.63 fps and mostly
clustered around 22-23 fps.

Second Shock Tetris JP result, also on the same local-read `.pdx`: across
110 non-zero windows, average was ~23.19 fps; after the same early transition
point, the steady section averaged ~22.64 fps across 93 windows. This confirms
the current local-read build is repeatable, but it is not a baseline/control.

Clarification: no clean Shock Tetris baseline has been collected yet for this
specific A/B. The next step is to build a real control by reverting only the
`0x35` local-read use while keeping the `0x95` local-read use, then rerun Shock
Tetris.

Control build prepared: `0x35` / `CMP A, [#nnnn]` was restored to
`MinxCPU_OnRead()`, while `0x95` / `TST [HL], #nn` still uses the local inline
read helper. Use this `.pdx` as the Shock Tetris control for isolating the
`0x35` change.

Shock Tetris JP `0x95`-only control result: across 104 non-zero 30-update
windows, average was ~22.24 fps, with 44 windows >=22 fps, 12 windows >=23 fps,
12 windows >=24 fps, 9 windows >=25 fps, and 0 windows >=27 fps. After the
same early transition point, the steady section averaged ~21.91 fps across
87 windows.

Comparison: the two-opcode local-read build (`0x35` + `0x95`) produced two
repeatable steady Shock Tetris samples at ~22.63 and ~22.64 fps. The `0x95`
only control is about 0.7 fps slower in the steady section, so `0x35` local
read appears to be a real win for Shock Tetris. Restored the `0x35` local-read
path in the active build.

Pokemon Pinball Mini Japan sample on the active two-opcode local-read build,
with C available via crank: across 46 non-zero 30-update windows, average was
~23.91 fps, with 39 windows >=22 fps, 21 windows >=23 fps, 16 windows >=24 fps,
13 windows >=25 fps, 11 windows >=26 fps, and 7 windows >=27 fps. After the
early/title transition section, the later gameplay section averaged ~22.77 fps
across 32 windows. This is a playable data point, not yet a clean A/B versus
keeper or `0x35`-only/`0x95`-only variants.

Pinball control build prepared: `0x35` / `CMP A, [#nnnn]` still uses the local
inline read helper, while `0x95` / `TST [HL], #nn` was restored to
`MinxCPU_OnRead()`. Use this `.pdx` to isolate whether the `0x95` local-read
path helped Pinball.

## Controls (2026-05-10)

Pokemon Mini **C** is now mapped to a Playdate crank angle zone in
`PokeMini_Playdate.c::handle_input()`: undock the crank and rotate into
60°-180° to hold C; move outside the zone or dock it to release C. This sends
`MINX_KEY_C` directly through `UIMenu_KeyEvent()` and intentionally bypasses
the joystick mapping table so older saved options where C was unmapped still
get the input. This avoids using the dock/undock mechanism itself as a gameplay
button.

Earlier A+B chord and pure dock/undock attempts were removed because they were
not suitable for repeated Pinball testing.

## Screen Scaling (2026-05-10)

The Playdate system menu exposes `Scale: 3x/3.5x`. `3x` is the default stable
integer-scale view: the 96x64 Pokemon Mini framebuffer becomes 288x192,
centered at (56, 24). `3.5x` is available as a larger experimental view:
336x224 centered at (32, 8), using an alternating 3/4-pixel expansion pattern
in both axes. Changing scale clears the full framebuffer before the next dirty
screen render so stale border pixels do not remain.

The 3x renderer intentionally keeps the older direct expansion path so the
default scale remains comparable to earlier performance baselines. The 3.5x
renderer uses the row-buffer expansion path because its alternating 3/4-pixel
pattern is less regular.

## CPU branch experiment (2026-05-10)

The scoped fast-fetch experiment in `MinxCPU_XX.c` tested neutral-to-slightly
worse on Togepi, so it was removed. A follow-up specialization of the hot
`E7` / `JNZ #ss` opcode caused in-game audio to disappear on device, so that
was also reverted. Keep the stock branch helper there unless a safer
correctness test is added.

## CE local-read experiment (2026-05-10)

Added a narrow local-read test for `CE D0` / `MOV A, [#nnnn]`, which is hot in
the opcode diagnostics for several games. First Togepi default-settings logs
looked positive and audio stayed working. Extending the same helper to the
neighboring read-only absolute loads `CE D1-D3` regressed Togepi badly, so
those were reverted. Keep only `CE D0` for now; CE writes and branch opcodes
stay on the stock helpers.

## CF stack local-read experiment (2026-05-10)

Testing a single stack read optimization for `CF B5` / `POP B`, which was hot
alongside `CF B1` in opcode diagnostics. The write-side `PUSH B` remains stock;
`POP B` now reads through a local direct-read helper and then increments SP in
the same order as `POP()`.

First Togepi logs with `CF B5` felt slightly smoother and did not obviously
regress. The matching single-op stack write test for `CF B1` / `PUSH B`
regressed badly, so it was reverted. Keep `CF B5` only for now.

## E7 / JNZ #ss local-fetch experiment — reverted (2026-05-11)

Tried specializing `0xE7` (`JNZ #ss`, ~35% of Togepi/Pinball top-level
dispatches) by inlining `Fetch8()` through a new `MinxCPU_XX_LocalFetch8()`
helper that uses `MinxCPU_XX_LocalRead` instead of `MinxCPU_OnRead`.
Implementation was byte-for-byte semantic match to upstream `Fetch8()`:
same bank logic, same `PC.W.L` post-increment, same `MinxCPU.IR` update.

Build size: `.text` grew by +128 bytes (114486 vs 114358 keeper). Hot
dispatcher addresses unchanged.

Result on Togepi JP, smooth LCD, first-three-levels: dropped to ~22 fps
steady (down from keeper's ~25.9). Audio worked, no visual glitches —
fully correct semantically. The +128-byte expansion was apparently
enough to shift dispatcher layout into a less favorable cache pattern.

Conclusion: reverted both the `MinxCPU_XX_LocalFetch8` helper and the
E7 use site. Local-fetch is *not* a useful pattern beyond what the
existing local-reads already buy us. The "fix the previous audio bug"
theory was wrong on two counts: (a) my conservative implementation
didn't break audio, and (b) even with audio working, perf doesn't pay
off. A previous Codex attempt may have been unrelatedly buggy or may
have hit the same I-cache cliff via a different code path.

## MinxPRC_Render_Mono `__attribute__((optimize("O3")))` — reverted (2026-05-11)

Tried per-function `-O3` on `MinxPRC_Render_Mono` (the BG + sprite
render loop, ~72 calls/sec, currently in the cold section). Reasoning:
narrow scope avoids the file-level `MinxPRC.c -O2` regression we
already saw, and Render_Mono has nested loops + lots of memory reads
that GCC `-O3` should unroll/inline aggressively.

Build size: `MinxPRC_Render_Mono` ballooned from 460 bytes to **9,730
bytes** (21×). GCC inlined `MinxPRC_OnRead` and `MinxPRC_DrawSprite8x8_Mono`
into the body, then unrolled the nested 8×96 BG loop. Total `.text`
grew from 114358 to 123262 (+8904).

Result on Togepi JP, smooth LCD, first-three-levels: ~24 fps avg
(down from keeper's ~25.9), with relatively *low variance* —
clustered tightly around 24.5-25.2 fps, peaks to ~26.6, stalls
17-22 fps. Subjectively user reported "felt smoother" than keeper
because of the tighter variance, even though average and peaks were
both lower.

Conclusion: reverted. The avg-fps regression (~−2) outweighed the
smoothness gain on raw benchmarks; ~+8.9 KB binary cost is also
real. The interesting finding: the keeper's **higher peaks come
with deeper variance**; experiments that touch this region of
hot/cold layout tend to flatten the curve in both directions. None
of the variants tested cleanly beat the keeper.

(Earlier note here had this entry at "~22 fps regression" — that
was a data mix-up with the E7 run. The actual Render_Mono `-O3`
result is what's recorded above.)

## MinxPRC_Sync `__attribute__((optimize("O3")))` — reverted (2026-05-11)

Last perf experiment. `MinxPRC_Sync` is the smallest remaining hot
target (248 bytes, ~256 calls/frame, currently in `.text.hot`).
Branch-heavy state machine with no loops, so `-O3` shouldn't
explode like it did for Render_Mono — risk profile much lower.

Build size: `.text` grew by only **+48 bytes total**, `MinxPRC_Sync`
went from 248 → 256 bytes (+8 bytes). Hot dispatcher byte-for-byte
identical to keeper.

Result on Togepi JP, smooth LCD, first-three-levels: ~24.5 fps avg
(down from keeper's ~25.9), strikingly similar shape to Render_Mono
above — cluster tight around 25.0-25.2 fps, peaks to 26.7, stalls
17-22. Lower variance than keeper.

Conclusion: reverted. The most diagnostic finding here: **Render_Mono
(+8,904 bytes) and SyncO3 (+48 bytes) produced nearly identical fps
profiles**. That tells us the regression isn't really about the size
of the change — it's that *any* perturbation in this region of the
I-cache shifts us off the keeper's specific sweet spot, and we land
in a similar nearby steady-state. Further tweaking would just shuffle
which scenes are slightly faster vs slower without lifting the
average.

Removed the `POKEMINI_O3` macro from `PokeMini.h` since no function
benefits from it in the final config. Future experimenters can
re-add if needed.

## Production ceiling reached (2026-05-11)

After E7 local-fetch (~4 fps regression), Render_Mono `-O3` (~2 fps
regression with smoother variance), and SyncO3 (~1.4 fps regression
with smoother variance) all failed to beat the keeper, we're at the
realistic perf ceiling for this combination of (Cortex-M7 Rev A,
4 KB I-cache, current PokeMini dispatcher architecture, current
Playdate SDK).

Key diagnostic insight from the last three experiments: **the keeper
config sits on a specific I-cache sweet spot**. Any perturbation
moves us off it into a nearby flatter steady-state with lower
variance but lower average. Render_Mono +8,904 bytes and SyncO3 +48
bytes produced nearly identical fps profiles — the size of the
change was almost irrelevant.

Cumulative wins delivered, in order:

1. Section consolidation + 4 KB alignment of `MinxCPU_Exec`.
2. `-Os` over `-O3` for the global Playdate build.
3. Custom `link_map.ld`, `POKEMINI_HOT_EXEC` placement.
4. `MinxTimers.c -O3` + `Hardware.c -O2` per-file overrides.
5. Local-read helpers for `0x35`, `CE D0`, `CF B5` (selectively).
6. Perf-keepalive (4× `getElapsedTime` per update + 1×/sec
   format-rich log) — empirically required.
7. Crank-as-C, 3x/3.5x scaling, LCD Soft/Fast toggle — UX, not perf.

Heavy Togepi ends up at ~25.9 fps display avg, ~27.2-27.8 fps in
steady stretches, with periodic stalls into the high teens during
game-driven busy-waits. The English fan-translation of Togepi
(measured 2026-05-10) lands in the same band — typical ~23-24 fps
steady, peaks ~25 — i.e. no meaningful difference vs. JP either
way (earlier drafts of these notes claimed English was faster; that
was unmeasured speculation, now retracted). Light scenes on either
hit native PM speed.

What does NOT work and should not be retried:

- E7 / JNZ #ss specialization (above).
- Render_Mono `-O3` attribute (above).
- File-level `MinxCPU_XX -O2` (~+10 KB, tanks I-cache).
- File-level `MinxPRC.c -O2` (similar).
- Computed-goto / threaded dispatcher (regressed).
- Broad `POKEMINI_CPU_FASTMEM` helper (regressed).
- `synccycles` > 512 (breaks PRC).
- `setRefreshRate(36)` to match PM (display can't sustain).

What might still work, in approximate effort/risk order:

- More opcode-specific local reads, *one at a time*, for opcodes
  identified as hot in the cross-game profile. Each adds ~+128 bytes
  and may regress; expect 0.0-0.3 fps net per addition.
- `__attribute__((optimize("O3")))` on individual *small* functions
  in the call graph (not Render_Mono — it's already huge). Maybe
  `MinxPRC_Sync` itself? It's only 248 bytes and called per batch.
- Dynarec / partial JIT for the cart-ROM execution path. Days of
  work, uncertain payoff, fundamentally bigger lift than anything
  attempted so far.

What probably won't work without firmware/SDK changes:

- True ITCM placement of the dispatcher (firmware doesn't expose).
- DTCM placement of PM_RAM for fast emulator state (same).
- Higher CPU clock (firmware-set).

A second-tier ceiling on the *smooth+3.5x* render path was pushed
later — see "Smooth-mode memory-bandwidth pass" below.

## Smooth-mode memory-bandwidth pass (2026-05-11)

### Trigger

User reported ~10% throughput gap between `LCD Mode: Fast / Scale: 3x`
and `LCD Mode: Soft / Scale: 3.5x` on heavy Togepi, and asked
whether the render code could be optimized. Pre-diagnostic guess
was "render is ~1% of CPU per the 2026-05-03 measurement, can't
be much" — but that number predated the 3.5x scale and analog
threshold/dither code, so it was stale.

### Diagnostic

Turned `PD_PERF_DIAG` back on. Per-second phase totals on the same
Togepi steady-state scene:

| Phase | Fast 3x | Smooth 3.5x | Δ |
| --- | --- | --- | --- |
| prc | ~125 ms | ~235 ms | **+110 ms** |
| render | ~13 ms | ~33 ms | +20 ms |
| total | ~1,470 ms | ~1,615 ms | +145 ms |
| fps | ~20.0 | ~18.2 | -10% |

So the slowdown is ~85% emulator-side, ~15% render-side. The prc
delta is `MinxLCD_DecayRefresh`, called every PM frame in
`LCDMODE_ANALOG`; the render delta is the analog row builder + the
3.5x `expand_pair_35x` bit-expansion loop.

### What didn't work: per-pixel ALU reduction

First attempt: replace `BitsActives[sh]` + `(P0*(4-level)+P1*level)>>2`
inside DecayRefresh with a precomputed `MinxLCD_DecayLUT[sh]`
(16 bytes, rebuilt in `MinxLCD_SetContrast`). Cleanly removed ~5
ALU ops per pixel from a 6144-iter inner loop run at 72 Hz.

Result: **0 fps gain**, prc unchanged (235 → 235 ms).

Why: the loop wasn't ALU-bound. Per iteration: 2 byte loads
(`LCDPixelsD`, `LCDPixelsAS`) + 2 byte stores (`LCDPixelsAS`,
`LCDPixelsA`). Working set 3×6144 = 18 KB; Cortex-M7 Rev A D-cache
is 4 KB. The CPU was already waiting on memory; collapsing the ALU
made no difference. **Cortex-M7 has enough dual-issue / pipelining
headroom that ALU work overlapped with memory stalls is free.**

The DecayLUT was retained as a small cleanup for non-Playdate
ports (gated `#ifndef TARGET_PLAYDATE`) — it's a real if tiny win
on platforms where the bottleneck isn't memory.

### What worked: skip the LCDPixelsA store + threshold off LCDPixelsAS

The actual lever was reducing memory traffic, not compute. Approach:

1. On Playdate, stop materializing the smoothed brightness buffer
   `LCDPixelsA` inside `MinxLCD_DecayRefresh` (one fewer byte store
   per pixel, -25% memory ops in the loop).
2. Render path samples the history nibble `LCDPixelsAS[i]` directly.
   For each of the 16 possible `sh` values, precompute "does this
   shade clear t_on / t_off" into two 16-byte LUTs
   (`pm_sh_to_high`, `pm_sh_to_mid`). Rebuild lazily when
   `Pixel0/1Intensity` change (cached `p0`/`p1` check, two int
   compares per `render_screen` call).
3. Replace `(srcA[i] >= t_on)` / `(srcA[i] >= t_off)` compares in
   `build_pm_row_bits` and `render_screen_3x` analog paths with
   `pm_sh_to_high[srcAS[i]]` / `pm_sh_to_mid[srcAS[i]]` lookups.

`LCDPixelsAS` was already allocated (in the second half of
`LCDPixelsA`'s heap block) but not exported in the header; added
an `extern` in `MinxLCD.h`.

Result on Togepi smooth+3.5x: prc **235 → 200 ms (-15%)**, render
33 → 29 ms (-12%, secondary effect from no longer reading
LCDPixelsA bytes that were in the working set), tim/cpu also
nudged down a bit (cache-pressure spillover). **Wall fps 18.2 →
19.2 (+5.5%).** Visual output pixel-identical (LUTs encode the
same math).

### Bonus: 256-entry LUT for `expand_pair_35x`

The 3.5x scale path called `expand_pair_35x` for every 2-byte PM
row chunk. The function ran a 16-iter loop with per-bit
conditionals and variable-width shifts (3 or 4 bits depending on
position) to produce 7 dst bytes from 16 src bits.

Replaced with a 256-entry `uint32_t` table: each src byte maps to
its 28-bit expansion (low 28 bits of a uint32; 1 KB rodata). Body
is then two table reads, four shifts/ORs, seven byte stores. Both
halves stitched at the byte-3 boundary via
`((e0 << 4) | (e1 >> 24))`.

Result: render **29 → 17 ms (-41%)**. Translated to wall fps:
~+0.2 (we're no longer render-bound; render is ~1% of total).

### Cumulative

| Metric | Baseline | After step 1 | After step 1+LUT |
| --- | --- | --- | --- |
| prc | ~235 ms | ~200 ms | ~204 ms |
| render | ~33 ms | ~29 ms | ~17 ms |
| total | ~1,615 ms | ~1,530 ms | ~1,517 ms |
| fps | ~18.2 | ~19.2 | ~19.4 |

**+6.6% throughput on heavy Togepi smooth+3.5x.** The smooth/3.5x
→ fast/3x gap closed from ~10% to ~3%. Fast 3x unchanged (the
DecayRefresh + analog render path isn't on its critical path).

### Calibration: diag-on numbers were inflated

When `PD_PERF_DIAG` was stripped and the same scene re-measured,
smooth+3.5x came in at ~23.5-24 fps steady, peaks 25.2. Back-
computing through the same comparison: pre-patch smooth+3.5x
without diag was probably ~22.5-23 fps. So the real-world gain
from option-1 + option-2 is closer to **+0.5-1 fps (~+3-5%)**,
not the +6.6% the diag-on A/B suggested.

The diag instrumentation itself was costing ~18% of throughput
(19.4 fps diag-on vs ~23.7 diag-off on the same code) — its
`getElapsedTime()` calls inside the per-batch Hardware.c loop sat
right on the hot path. Take that into account when reading any
absolute fps numbers in this section: the deltas between modes
are still valid (diag adds the same tax in both), but the
post-patch ceiling on the final shipped binary is `~23.5-24 fps
avg / 25.2 peak`, putting smooth+3.5x within ~2 fps of fast+3x.

If you turn diag back on for future investigation, expect the
fps floor to drop into the high teens again — the diag overhead
is not a bug, it's the cost of measurement and the only honest
comparison is mode-vs-mode within the same diag setting.

### Lesson worth remembering

When a perf hypothesis targets ALU reduction, measure the actual
bottleneck before committing. On M7 + small caches + streaming
byte buffers, **memory bandwidth is almost always the limit**, and
collapsing arithmetic in a memory-bound loop is free for the
hardware (and so for fps). The win comes from reducing total
memory traffic — fewer loads/stores, smaller working set, or
better cache locality.

### Files touched

- `source/MinxLCD.h` — export `LCDPixelsAS`.
- `source/MinxLCD.c` — DecayRefresh: skip LCDPixelsA store under
  `#ifndef TARGET_PLAYDATE`. DecayLUT/BitsActives also gated
  there (no consumer on Playdate).
- `platform/playdate/PokeMini_Playdate.c` — added
  `pm_sh_to_high`/`pm_sh_to_mid` LUTs + lazy rebuild; added
  `expand_lut_35x[256]` populated in `init_expand_lut`; rewrote
  analog branches of `build_pm_row_bits` and `render_screen_3x` to
  read `LCDPixelsAS` via LUT; replaced `expand_pair_35x` body with
  table-lookup unpacking.

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
