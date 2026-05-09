/*
  PokeMini - Pokemon-Mini Emulator
  Copyright (C) 2009-2015  JustBurn

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pd_api.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "PokeMini.h"
#include "Hardware.h"
#include "Joystick.h"
#include "Video_x1.h"
#include "UI.h"

static PlaydateAPI *pd;

const char *AppName = "PokeMini " PokeMini_Version " Playdate";

// Platform menu (required by UI.c)
int UIItems_PlatformC(int index, int reason);
TUIMenu_Item UIItems_Platform[] = {
	PLATFORMDEF_GOBACK,
	PLATFORMDEF_SAVEOPTIONS,
	PLATFORMDEF_END(UIItems_PlatformC)
};
int UIItems_PlatformC(int index, int reason)
{
	if (reason == UIMENU_CANCEL) UIMenu_PrevMenu();
	return 1;
}

// Playdate: 400x240 1-bit monochrome display
// Pokemon Mini native screen: 96x64 pixels
// Integer 3x scale -> 288x192, centered at (56, 24) on the Playdate screen
#define PM_W     96
#define PM_H     64
#define PM_SCALE 3
#define SCREEN_X ((LCD_COLUMNS - PM_W * PM_SCALE) / 2)   // 56
#define SCREEN_Y ((LCD_ROWS    - PM_H * PM_SCALE) / 2)   // 24

// ROM buffer allocated via Playdate file API (fopen is unsupported on device)
static uint8_t *rom_buf = NULL;

// Audio source handle
static SoundSource *audio_source = NULL;

// Playdate physical button names used by the joystick subsystem
static char *PD_KeysNames[] = {
	"Off",    // -1
	"A",      //  0
	"B",      //  1
	"Up",     //  2
	"Down",   //  3
	"Left",   //  4
	"Right",  //  5
	"Accel",  //  6 — accelerometer-driven shake
};

// PM key order: Menu, A, B, C, Up, Down, Left, Right, Power, Shake
static int PD_KeysMapping[] = {
	-1,  // Menu  -> unmapped (handled by system menu)
	 0,  // A     -> button A
	 1,  // B     -> button B
	-1,  // C     -> unmapped
	 2,  // Up    -> D-pad Up
	 3,  // Down  -> D-pad Down
	 4,  // Left  -> D-pad Left
	 5,  // Right -> D-pad Right
	-1,  // Power -> unmapped
	 6,  // Shake -> accelerometer (squared-magnitude threshold)
};

// Playdate audio callback: fill the left channel with generated PM audio samples.
// MinxAudio_GenerateEmulatedS16 reads only a few emulator state variables and is
// safe to call from the audio thread when using POKEMINI_GENSOUND.
static int audio_callback(void *context, int16_t *left, int16_t *right, int len)
{
	(void)context;
	(void)right;
	MinxAudio_GenerateEmulatedS16(left, len, 1);
	return 1;
}

// Load a .min ROM from the Playdate filesystem into a heap buffer, then hand
// the buffer to the emulator via PokeMini_SetMINMem.  We use pd->file->* here
// because the standard fopen() is not available on Playdate hardware.
//
// kFileRead | kFileReadData: search the data side (which covers the cross-app
// /Shared/ folder added in SDK 2.4) AND the bundle. With kFileRead alone the
// API only searches the read-only pdx bundle, which silently misses every
// ROM the user dropped into /Shared/Emulation/pm/games/.
static int load_rom(const char *path)
{
	SDFile *f = pd->file->open(path, kFileRead | kFileReadData);
	if (!f) {
		const char *err = pd->file->geterr();
		pd->system->logToConsole("%s: file->open(%s) failed: %s",
			AppName, path, err ? err : "(no error)");
		return 0;
	}

	pd->file->seek(f, 0, SEEK_END);
	int size = pd->file->tell(f);
	pd->file->seek(f, 0, SEEK_SET);

	if (size <= 0) {
		pd->file->close(f);
		return 0;
	}

	// Free the previous ROM's buffer before allocating a new one — picking a
	// second ROM without this leaks the first.
	if (rom_buf) {
		free(rom_buf);
		rom_buf = NULL;
	}

	rom_buf = (uint8_t *)malloc((size_t)size);
	if (!rom_buf) {
		pd->file->close(f);
		return 0;
	}

	pd->file->read(f, rom_buf, (unsigned int)size);
	pd->file->close(f);

	return PokeMini_SetMINMem(rom_buf, size);
}

// Poll all Playdate buttons and forward press/release events to the joystick
// subsystem, which maps them to Pokemon Mini keys via PD_KeysMapping.
// A vigorous device shake (accelerometer magnitude past a threshold) emits a
// PM "Shake" pulse — a more natural fit than the crank ever was.
static void handle_input(void)
{
	PDButtons cur, pushed, released;
	pd->system->getButtonState(&cur, &pushed, &released);

	if (pushed   & kButtonA)     JoystickButtonsEvent(0, 1);
	if (released & kButtonA)     JoystickButtonsEvent(0, 0);
	if (pushed   & kButtonB)     JoystickButtonsEvent(1, 1);
	if (released & kButtonB)     JoystickButtonsEvent(1, 0);
	if (pushed   & kButtonUp)    JoystickButtonsEvent(2, 1);
	if (released & kButtonUp)    JoystickButtonsEvent(2, 0);
	if (pushed   & kButtonDown)  JoystickButtonsEvent(3, 1);
	if (released & kButtonDown)  JoystickButtonsEvent(3, 0);
	if (pushed   & kButtonLeft)  JoystickButtonsEvent(4, 1);
	if (released & kButtonLeft)  JoystickButtonsEvent(4, 0);
	if (pushed   & kButtonRight) JoystickButtonsEvent(5, 1);
	if (released & kButtonRight) JoystickButtonsEvent(5, 0);

	// Accelerometer returns x/y/z in g-units. At rest the magnitude is ~1.0
	// (gravity); a confident shake pushes it well past 1.0. Compare squared
	// magnitudes to avoid the sqrt — threshold tuned by feel on hardware.
	// Cooldown prevents one shake gesture from firing multiple PM events.
	static int shake_cooldown = 0;
	if (shake_cooldown > 0) shake_cooldown--;

	float ax, ay, az;
	pd->system->getAccelerometer(&ax, &ay, &az);
	float mag2 = ax * ax + ay * ay + az * az;
	if (mag2 > 2.5f && shake_cooldown == 0) {
		JoystickButtonsEvent(6, 1);
		JoystickButtonsEvent(6, 0);
		shake_cooldown = 6;  // ~200 ms at 30 fps
	}
}

// 8 PM pixels (1 bit each, MSB = leftmost) expand to 24 Playdate framebuffer
// bits at 3x horizontal scale.  Precomputed once at startup so each PM byte
// becomes 3 byte stores instead of 24 read-modify-writes.
static uint8_t expand_lut[256][3];

static void init_expand_lut(void)
{
	for (int i = 0; i < 256; i++) {
		uint32_t bits = 0;
		for (int b = 0; b < 8; b++) {
			if ((i >> (7 - b)) & 1) {
				// Set 3 consecutive bits at the matching position in the 24-bit
				// output (MSB-first to match Playdate framebuffer layout).
				bits |= (uint32_t)0x7 << (24 - 3 - b * 3);
			}
		}
		expand_lut[i][0] = (uint8_t)(bits >> 16);
		expand_lut[i][1] = (uint8_t)(bits >> 8);
		expand_lut[i][2] = (uint8_t)(bits);
	}
}

// Blit the PM 96x64 frame into the Playdate 400x240 1-bit framebuffer at 3x
// integer scale, centered at (SCREEN_X, SCREEN_Y).  SCREEN_X (56) is byte-
// aligned (56 = 7*8), so each row writes 36 bytes starting at fb + row*RS + 7.
// 0 in LCDPixelsD/A = pixel off = light = white = bit set on Playdate.
//
// PM games fake gray shades by toggling pixels every native frame (72 Hz).
// On a 1-bit panel that's just visible flicker. To suppress it we run the
// emulator in LCDMODE_ANALOG: MinxLCD_DecayRefresh keeps a 4-frame history
// per pixel and writes a smoothed brightness value (0..255) into LCDPixelsA,
// interpolated between MinxLCD.Pixel0Intensity and Pixel1Intensity.
//
// Thresholds split that into three buckets per pixel:
//   - >= t_on  (~3 frames lit)  -> solid on
//   - <  t_off (~1 frame  lit)  -> solid off
//   - between                   -> checkerboard dither
// The 4-frame history makes moving "gray" sprites fade in/out smoothly
// instead of dithering on both edges as they slide.
static void render_screen(void)
{
	if (!LCDDirty) return;
	LCDDirty = 0;

	uint8_t *fb = pd->graphics->getFrame();
	const int byte_x = SCREEN_X / 8;  // 7
	const int pm_bytes = PM_W / 8;    // 12

	// Thresholds adapt to the game's current contrast setting (Pixel0/Pixel1
	// intensities change with MinxLCD.Contrast). Boundaries at level 1.5 and
	// level 2.5 of the 5-level interpolation: 3/8 and 5/8 of the span.
	const int p0 = MinxLCD.Pixel0Intensity;
	const int p1 = MinxLCD.Pixel1Intensity;
	const int span = p1 - p0;
	const uint8_t t_off = (uint8_t)(p0 + (3 * span) / 8);
	const uint8_t t_on  = (uint8_t)(p0 + (5 * span) / 8);

	for (int y = 0; y < PM_H; y++) {
		const uint8_t *srcA = &LCDPixelsA[y * PM_W];
		uint8_t *dst0 = fb + (SCREEN_Y + y * PM_SCALE)     * LCD_ROWSIZE + byte_x;
		uint8_t *dst1 = dst0 + LCD_ROWSIZE;
		uint8_t *dst2 = dst1 + LCD_ROWSIZE;

		const uint8_t dither = (y & 1) ? 0xAA : 0x55;

		for (int bx = 0; bx < pm_bytes; bx++) {
			// "high" byte: pixels brighter than the on threshold (always on).
			uint8_t high =
				((srcA[0] >= t_on) << 7) | ((srcA[1] >= t_on) << 6) |
				((srcA[2] >= t_on) << 5) | ((srcA[3] >= t_on) << 4) |
				((srcA[4] >= t_on) << 3) | ((srcA[5] >= t_on) << 2) |
				((srcA[6] >= t_on) << 1) | ((srcA[7] >= t_on)     );
			// "mid" byte: pixels brighter than the off threshold (dither candidates).
			uint8_t mid =
				((srcA[0] >= t_off) << 7) | ((srcA[1] >= t_off) << 6) |
				((srcA[2] >= t_off) << 5) | ((srcA[3] >= t_off) << 4) |
				((srcA[4] >= t_off) << 3) | ((srcA[5] >= t_off) << 2) |
				((srcA[6] >= t_off) << 1) | ((srcA[7] >= t_off)     );
			srcA += 8;

			// On = always-on pixels, plus mid pixels gated by dither.
			// Invert to get "off" mask (Playdate: bit set = white = pixel off).
			uint8_t b = (uint8_t)~(high | (mid & dither));

			// Expand to 3 Playdate bytes, replicate across 3 vertical-scale rows.
			const uint8_t *e = expand_lut[b];
			dst0[0] = e[0]; dst0[1] = e[1]; dst0[2] = e[2];
			dst1[0] = e[0]; dst1[1] = e[1]; dst1[2] = e[2];
			dst2[0] = e[0]; dst2[1] = e[1]; dst2[2] = e[2];
			dst0 += 3; dst1 += 3; dst2 += 3;
		}
	}

	pd->graphics->markUpdatedRows(SCREEN_Y, SCREEN_Y + PM_H * PM_SCALE - 1);
}

// --- ROM picker -----------------------------------------------------------
//
// Lists *.min files in /Shared/Emulation/pm/games/ (CrankBoy convention) and
// lets the user pick one with D-pad + A. If the directory is empty, falls
// back to the bundled boot.min. Rough/testing UI, not final.

#define MAX_ROMS 64
#define MAX_ROM_NAME 64

static const char *ROM_DIR = "/Shared/Emulation/pm/games/";

static char rom_names[MAX_ROMS][MAX_ROM_NAME];
static int rom_count = 0;
static int rom_cursor = 0;
static int rom_view_top = 0;

typedef enum { MODE_PICKER, MODE_EMULATOR } AppMode;
static AppMode app_mode = MODE_PICKER;

static LCDFont *picker_font = NULL;

static int has_min_suffix(const char *name)
{
	size_t len = strlen(name);
	if (len < 5) return 0;  // need at least "x.min"
	const char *e = name + len - 4;
	return e[0] == '.'
	    && (e[1] == 'm' || e[1] == 'M')
	    && (e[2] == 'i' || e[2] == 'I')
	    && (e[3] == 'n' || e[3] == 'N');
}

static int rom_listfiles_seen = 0;

static void rom_listfiles_cb(const char *filename, void *ctx)
{
	(void)ctx;
	rom_listfiles_seen++;
	pd->system->logToConsole("%s:   listfiles entry: '%s'", AppName, filename);
	if (rom_count >= MAX_ROMS) return;
	if (!has_min_suffix(filename)) return;
	if (strlen(filename) >= MAX_ROM_NAME) return;
	strcpy(rom_names[rom_count], filename);
	rom_count++;
}

// pd->file->mkdir is non-recursive, so create each path segment explicitly.
// Each call silently no-ops if the directory already exists.
// /Shared/ paths are routed to the cross-app shared folder (SDK 2.4+); mkdir
// works correctly with these paths as of SDK 2.4.1.
static void ensure_rom_dir(void)
{
	pd->file->mkdir("/Shared");
	pd->file->mkdir("/Shared/Emulation");
	pd->file->mkdir("/Shared/Emulation/pm");
	pd->file->mkdir("/Shared/Emulation/pm/games");
}

static void scan_rom_dir(void)
{
	rom_count = 0;
	rom_cursor = 0;
	rom_view_top = 0;
	rom_listfiles_seen = 0;

	ensure_rom_dir();

	// Verify the directory we're about to scan actually exists. If stat
	// returns an error or isdir==0 it usually means /Shared/ access didn't
	// route as expected (older SDK firmware? sandbox quirk?).
	FileStat st;
	int sr = pd->file->stat(ROM_DIR, &st);
	if (sr != 0) {
		const char *err = pd->file->geterr();
		pd->system->logToConsole("%s: stat(%s) failed (%d): %s",
			AppName, ROM_DIR, sr, err ? err : "(no error)");
	} else {
		pd->system->logToConsole("%s: stat(%s) ok isdir=%d size=%u",
			AppName, ROM_DIR, st.isdir, st.size);
	}

	int lr = pd->file->listfiles(ROM_DIR, rom_listfiles_cb, NULL, 0);
	if (lr != 0) {
		const char *err = pd->file->geterr();
		pd->system->logToConsole("%s: listfiles(%s) returned %d: %s",
			AppName, ROM_DIR, lr, err ? err : "(no error)");
	}
	pd->system->logToConsole("%s: scan: %d entr(y/ies) seen, %d kept as *.min",
		AppName, rom_listfiles_seen, rom_count);
}

static void render_picker(void)
{
	pd->graphics->clear(kColorWhite);
	if (picker_font) pd->graphics->setFont(picker_font);
	pd->graphics->setDrawMode(kDrawModeCopy);

	const char *title = "PokeMini  -  Select ROM";
	pd->graphics->drawText(title, strlen(title), kASCIIEncoding, 16, 12);

	const int row_h = 18;
	const int list_y = 44;
	const int max_visible = 9;

	if (rom_cursor < rom_view_top) rom_view_top = rom_cursor;
	if (rom_cursor >= rom_view_top + max_visible)
		rom_view_top = rom_cursor - max_visible + 1;

	for (int i = 0; i < max_visible && rom_view_top + i < rom_count; i++) {
		int idx = rom_view_top + i;
		int y = list_y + i * row_h;
		const char *name = rom_names[idx];

		if (idx == rom_cursor) {
			pd->graphics->fillRect(8, y - 2, LCD_COLUMNS - 16, row_h, kColorBlack);
			pd->graphics->setDrawMode(kDrawModeFillWhite);
		} else {
			pd->graphics->setDrawMode(kDrawModeCopy);
		}
		pd->graphics->drawText(name, strlen(name), kASCIIEncoding, 16, y);
	}
	pd->graphics->setDrawMode(kDrawModeCopy);

	const char *hint = "Up/Down: select    A: play";
	pd->graphics->drawText(hint, strlen(hint), kASCIIEncoding, 16, LCD_ROWS - 22);

	pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
}

// Wire a freshly-loaded ROM into the emulator and switch into emulation mode.
// path == NULL means "boot with FreeBIOS only, no cart" — used when
// /Shared/Emulation/pm/games/ is empty. The FreeBIOS shows its own splash
// in that case, which is what the user sees when no ROM is loaded.
//
// Mirrors PokeMini_LoadFromCommandLines: soft reset (the hard-reset BIOS
// path leaves SYS_CTRL3 in a state where the cart can stall before enabling
// the PRC 72 Hz interrupt), then ApplyChanges to commit any CommandLine
// tweaks the picker may have changed.
static void start_emulation_with_rom(const char *path)
{
	if (path == NULL) {
		pd->system->logToConsole("%s: starting with FreeBIOS only (no ROM)", AppName);
	} else if (!load_rom(path)) {
		pd->system->logToConsole("%s: load_rom(%s) failed - falling back to FreeBIOS",
			AppName, path);
	} else {
		pd->system->logToConsole("%s: ROM loaded: %s size=%d mask=0x%X",
			AppName, path, PM_ROM_Size, PM_ROM_Mask);
	}

	PokeMini_Reset(0);
	PokeMini_ApplyChanges();

	// Repaint the whole framebuffer black so the border around the PM screen
	// stays black without per-frame redraws (and overwrites the picker UI).
	uint8_t *fb = pd->graphics->getFrame();
	memset(fb, 0x00, (size_t)(LCD_ROWSIZE * LCD_ROWS));
	pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);

	LCDDirty = 1;
	app_mode = MODE_EMULATOR;
}

// Switch back to the picker. Called from the Playdate system menu so the
// player can change ROM without quitting the app. We don't rescan
// /Shared/... because connecting USB to add ROMs suspends the app — when
// the user re-launches, scan_rom_dir runs again from kEventInit.
static void return_to_picker(void)
{
	rom_cursor = 0;
	rom_view_top = 0;
	app_mode = MODE_PICKER;
}

static void menu_item_picker_cb(void *userdata)
{
	(void)userdata;
	return_to_picker();
}

static void picker_update(void)
{
	PDButtons cur, pushed, released;
	pd->system->getButtonState(&cur, &pushed, &released);
	(void)cur; (void)released;

	if (rom_count > 0) {
		if (pushed & kButtonUp)
			rom_cursor = (rom_cursor - 1 + rom_count) % rom_count;
		if (pushed & kButtonDown)
			rom_cursor = (rom_cursor + 1) % rom_count;

		if (pushed & kButtonA) {
			char path[MAX_ROM_NAME + 64];
			snprintf(path, sizeof(path), "%s%s", ROM_DIR, rom_names[rom_cursor]);
			start_emulation_with_rom(path);
			return;
		}
	}

	render_picker();
}

// Update callback: called by the Playdate runtime at 30 fps (target).
// Pokemon Mini runs natively at ~72 Hz. To keep emulated time matched to real
// time we need 72/30 = 2.4 PM frames per display frame on average. Use an
// integer fractional accumulator (numerator 12, denominator 5) to pace this
// exactly: across every 5 update calls we run 12 PM frames (pattern 2,2,3,2,3).
// When display FPS dips below 30 the emulator runs proportionally slower; this
// is preferable to time-based catch-up which trades display responsiveness for
// emulation accuracy and tends to spiral under sustained load.
static int update(void *userdata)
{
	(void)userdata;

	if (app_mode == MODE_PICKER) {
		picker_update();
		return 1;
	}

	static int frame_accum = 0;
	frame_accum += 12;
	int pm_frames = frame_accum / 5;
	frame_accum -= pm_frames * 5;

	for (int i = 0; i < pm_frames; i++) PokeMini_EmulateFrame();

	handle_input();
	render_screen();

	return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg)
{
	(void)arg;

	if (event == kEventInit) {
		pd = playdate;

		// 30 fps display rate; we emulate 2 PM frames per call
		pd->display->setRefreshRate(30);

		// Accelerometer powers the PM "Shake" input. Off by default to save
		// battery; enabling adds a small idle draw but it's the only way
		// getAccelerometer returns useful data.
		pd->system->setPeripheralsEnabled(kAccelerometer);

		init_expand_lut();

		// Configure emulator defaults suitable for Playdate
		CommandLineInit();
		CommandLine.sound      = 1;
		CommandLine.lcdfilter  = 0;
		// LCDMODE_ANALOG so MinxLCD_DecayRefresh writes a 4-frame smoothed
		// brightness into LCDPixelsA (Hardware.c:335). render_screen thresholds
		// that to suppress flicker and give moving "gray" sprites a soft fade
		// instead of dither smear. See render_screen for the threshold logic.
		CommandLine.lcdmode    = LCDMODE_ANALOG;
		CommandLine.synccycles = 512;

		JoystickSetup("Playdate", 0, 30000, PD_KeysNames, 7, PD_KeysMapping);

		// 1x video spec (96x64 output), 16bpp, no LCD filter, 2-shade mode
		if (!PokeMini_SetVideo((TPokeMini_VideoSpec *)&PokeMini_Video1x1, 16,
		                       CommandLine.lcdfilter, CommandLine.lcdmode)) {
			pd->system->error("PokeMini_SetVideo failed");
			return 1;
		}

		// POKEMINI_GENSOUND: synthesizes audio from CPU state without a FIFO,
		// safe to call from the separate Playdate audio thread
		if (!PokeMini_Create(POKEMINI_GENSOUND, 0)) {
			pd->system->error("PokeMini_Create failed");
			return 1;
		}

		PokeMini_LoadFreeBIOS();
		PokeMini_UseDefaultCallbacks();

		UIMenu_Init();

		// Load a font for the picker. If this fails the picker still runs
		// but text won't render — fine for a fallback/diagnostic path.
		const char *font_err = NULL;
		picker_font = pd->graphics->loadFont(
			"/System/Fonts/Asheville-Sans-14-Light.pft", &font_err);
		if (!picker_font) {
			pd->system->logToConsole("%s: font load failed: %s",
				AppName, font_err ? font_err : "(null)");
		}

		// Start the audio source up front so we don't have to thread it
		// through the picker -> emulator transition. While the picker is
		// up MinxAudio's state is idle so the source emits silence.
		audio_source = pd->sound->addSource(audio_callback, NULL, 0);

		// System menu item: lets the player return to the ROM picker without
		// quitting the app. Persists for the lifetime of the process.
		pd->system->addMenuItem("ROM Picker", menu_item_picker_cb, NULL);

		// Scan /Shared/Emulation/pm/games/ for *.min ROMs.
		// 0 ROMs -> boot FreeBIOS only (its own no-cart splash).
		// 1 ROM  -> auto-load it; no point showing a one-item picker.
		// 2+     -> show the picker.
		scan_rom_dir();
		pd->system->logToConsole("%s: %d ROM(s) in %s",
			AppName, rom_count, ROM_DIR);

		if (rom_count == 0) {
			start_emulation_with_rom(NULL);
		} else if (rom_count == 1) {
			char path[MAX_ROM_NAME + 64];
			snprintf(path, sizeof(path), "%s%s", ROM_DIR, rom_names[0]);
			start_emulation_with_rom(path);
		} else {
			app_mode = MODE_PICKER;
		}

		pd->system->setUpdateCallback(update, pd);

	} else if (event == kEventTerminate) {
		if (audio_source) {
			pd->sound->removeSource(audio_source);
			audio_source = NULL;
		}

		UIMenu_Destroy();

		// Attempt EEPROM save; works on simulator, silently skipped on device
		// (fopen is unsupported on hardware - add PokeMini_CustomSaveEEPROM for
		// a full device implementation using pd->file->* APIs)
		PokeMini_SaveFromCommandLines(1);

		PokeMini_Destroy();

		if (rom_buf) {
			free(rom_buf);
			rom_buf = NULL;
		}

		// pd->graphics->freeFont exists in the SDK but the Playdate runtime
		// reclaims everything on app exit — leaving the font handle is fine.
	}

	return 0;
}
