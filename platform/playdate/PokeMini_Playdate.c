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
#include "MinxCPU.h"
#include "MinxIO.h"
#include "Joystick.h"
#include "Video_x1.h"
#include "UI.h"

static PlaydateAPI *pd;

#ifdef PD_PERF_DIAG
unsigned int PDPerf_CpuUs = 0;
unsigned int PDPerf_TimersUs = 0;
unsigned int PDPerf_PrcUs = 0;
unsigned int PDPerf_AudioUs = 0;
unsigned int PDPerf_CeUs = 0;
unsigned int PDPerf_CfUs = 0;

static unsigned int pdperf_emu_us = 0;
static unsigned int pdperf_input_us = 0;
static unsigned int pdperf_render_us = 0;
static unsigned int pdperf_update_us = 0;
static unsigned int pdperf_updates = 0;
static unsigned int pdperf_pm_frames = 0;

unsigned int PDPerf_NowUs(void)
{
	return (unsigned int)(pd->system->getElapsedTime() * 1000000.0f);
}

static void pdperf_log_and_reset(void)
{
	unsigned int accounted =
		pdperf_emu_us + pdperf_input_us + pdperf_render_us;
	unsigned int misc = (pdperf_update_us > accounted)
		? pdperf_update_us - accounted : 0;

	pd->system->logToConsole(
		"diag: upd=%u pm=%u total=%uus emu=%u cpu=%u ce=%u cf=%u tim=%u prc=%u aud=%u input=%u render=%u misc=%u",
		pdperf_updates, pdperf_pm_frames, pdperf_update_us,
		pdperf_emu_us, PDPerf_CpuUs, PDPerf_CeUs, PDPerf_CfUs,
		PDPerf_TimersUs, PDPerf_PrcUs, PDPerf_AudioUs,
		pdperf_input_us, pdperf_render_us, misc);

	PDPerf_CpuUs = 0;
	PDPerf_CeUs = 0;
	PDPerf_CfUs = 0;
	PDPerf_TimersUs = 0;
	PDPerf_PrcUs = 0;
	PDPerf_AudioUs = 0;
	pdperf_emu_us = 0;
	pdperf_input_us = 0;
	pdperf_render_us = 0;
	pdperf_update_us = 0;
	pdperf_updates = 0;
	pdperf_pm_frames = 0;
}
#endif

#ifdef PD_OPCODE_DIAG
#define PD_OPDIAG_TOP_N 8
#define PD_OPDIAG_LOG_UPDATES 150

static const char *const pdopdiag_names[MINXCPU_OPDIAG_TABLES] = {
	"xx", "ce", "cf", "spce", "spcf"
};

static void pdopdiag_log_table(int table)
{
	uint32_t total = 0;
	uint32_t top_counts[PD_OPDIAG_TOP_N] = {0};
	uint8_t top_opcodes[PD_OPDIAG_TOP_N] = {0};

	for (int opcode = 0; opcode < 256; opcode++) {
		uint32_t count = MinxCPU_OpcodeDiag[table][opcode];
		total += count;
		if (count == 0 || count <= top_counts[PD_OPDIAG_TOP_N - 1])
			continue;

		for (int slot = 0; slot < PD_OPDIAG_TOP_N; slot++) {
			if (count > top_counts[slot]) {
				for (int move = PD_OPDIAG_TOP_N - 1; move > slot; move--) {
					top_counts[move] = top_counts[move - 1];
					top_opcodes[move] = top_opcodes[move - 1];
				}
				top_counts[slot] = count;
				top_opcodes[slot] = (uint8_t)opcode;
				break;
			}
		}
	}

	if (total == 0)
		return;

	pd->system->logToConsole(
		"opdiag: %s total=%lu top=%02X:%lu %02X:%lu %02X:%lu %02X:%lu %02X:%lu %02X:%lu %02X:%lu %02X:%lu",
		pdopdiag_names[table], (unsigned long)total,
		top_opcodes[0], (unsigned long)top_counts[0],
		top_opcodes[1], (unsigned long)top_counts[1],
		top_opcodes[2], (unsigned long)top_counts[2],
		top_opcodes[3], (unsigned long)top_counts[3],
		top_opcodes[4], (unsigned long)top_counts[4],
		top_opcodes[5], (unsigned long)top_counts[5],
		top_opcodes[6], (unsigned long)top_counts[6],
		top_opcodes[7], (unsigned long)top_counts[7]);
}

static void pdopdiag_log_and_reset(unsigned int updates)
{
	pd->system->logToConsole("opdiag: window=%u updates", updates);
	for (int table = 0; table < MINXCPU_OPDIAG_TABLES; table++)
		pdopdiag_log_table(table);
	MinxCPU_OpcodeDiagReset();
}
#endif

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
#define PM_W     96
#define PM_H     64

#define SCALE_3X_W  (PM_W * 3)
#define SCALE_3X_H  (PM_H * 3)
#define SCALE_3X_X  ((LCD_COLUMNS - SCALE_3X_W) / 2)  // 56
#define SCALE_3X_Y  ((LCD_ROWS    - SCALE_3X_H) / 2)  // 24

#define SCALE_35X_W  336
#define SCALE_35X_H  224
#define SCALE_35X_X  ((LCD_COLUMNS - SCALE_35X_W) / 2)  // 32
#define SCALE_35X_Y  ((LCD_ROWS    - SCALE_35X_H) / 2)  // 8

enum {
	RENDER_SCALE_3X,
	RENDER_SCALE_35X,
};

// ROM buffer allocated via Playdate file API (fopen is unsupported on device)
static uint8_t *rom_buf = NULL;

// App mode is declared here (rather than down by the picker code) because the
// audio_callback below reads it across threads to silence output while the
// picker is up. volatile so the audio thread sees writes from the main thread.
typedef enum { MODE_PICKER, MODE_EMULATOR } AppMode;
static volatile AppMode app_mode = MODE_PICKER;

// Set on kEventPause (system menu opens), cleared on kEventResume. The audio
// thread checks this to silence output during the system menu. volatile for
// the same reason as app_mode — written on the main thread, read on audio.
static volatile int emu_paused = 0;

// Audio source handle
static SoundSource *audio_source = NULL;

static int audio_callback(void *context, int16_t *left, int16_t *right, int len);

static void stop_audio_source(void)
{
	if (audio_source) {
		pd->sound->removeSource(audio_source);
		audio_source = NULL;
	}
}

static void start_audio_source(void)
{
	stop_audio_source();
	pd->sound->setOutputsActive(1, 1);

	audio_source = pd->sound->addSource(audio_callback, NULL, 1);
	if (audio_source) {
		pd->sound->source->setVolume(audio_source, 1.0f, 1.0f);
		pd->sound->setOutputsActive(1, 1);
		pd->system->logToConsole("%s: audio source started", AppName);
	} else {
		pd->system->logToConsole("%s: audio source creation failed", AppName);
	}
}

// System menu item for LCD quality/performance switching.
static PDMenuItem *lcd_mode_menu_item = NULL;
static const char *lcd_mode_options[] = { "Soft", "Fast" };
static PDMenuItem *screen_scale_menu_item = NULL;
static const char *screen_scale_options[] = { "3x", "3.5x" };
static int screen_scale_mode = RENDER_SCALE_3X;

// C is held while the crank sits in this undocked angle zone. This avoids
// using the dock/undock mechanism itself as a gameplay button.
#define C_CRANK_ANGLE_MIN 60.0f
#define C_CRANK_ANGLE_MAX 180.0f

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
	-1,  // C     -> direct crank angle hold in handle_input()
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
//
// We gate on app_mode: when the picker is up the emulator isn't being stepped,
// but MinxAudio.Volume and the Tmr3 register state still hold whatever the
// game last set them to — so without this gate the audio thread would keep
// emitting the last tone indefinitely after returning to the picker.
static int audio_callback(void *context, int16_t *left, int16_t *right, int len)
{
	(void)context;
	if (len <= 0) return 0;

	if (app_mode != MODE_EMULATOR || emu_paused) {
		memset(left, 0, (size_t)len * sizeof(*left));
		if (right) memset(right, 0, (size_t)len * sizeof(*right));
		return 1;
	}

	MinxAudio_GenerateEmulatedS16(left, len, 1);
	if (right) memcpy(right, left, (size_t)len * sizeof(*left));
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

	static int c_crank_pressed = 0;
	int c_crank_now = 0;
	if (!pd->system->isCrankDocked()) {
		float crank_angle = pd->system->getCrankAngle();
		c_crank_now = (crank_angle >= C_CRANK_ANGLE_MIN &&
		               crank_angle <= C_CRANK_ANGLE_MAX);
	}
	if (c_crank_now != c_crank_pressed) {
		UIMenu_KeyEvent(MINX_KEY_C, c_crank_now);
		c_crank_pressed = c_crank_now;
	}

	if (pushed   & kButtonUp)    JoystickButtonsEvent(2, 1);
	if (released & kButtonUp)    JoystickButtonsEvent(2, 0);
	if (pushed   & kButtonDown)  JoystickButtonsEvent(3, 1);
	if (released & kButtonDown)  JoystickButtonsEvent(3, 0);
	if (pushed   & kButtonLeft)  JoystickButtonsEvent(4, 1);
	if (released & kButtonLeft)  JoystickButtonsEvent(4, 0);
	if (pushed   & kButtonRight) JoystickButtonsEvent(5, 1);
	if (released & kButtonRight) JoystickButtonsEvent(5, 0);

	// Shake input via the accelerometer. Hardware samples at ~50 Hz and on
	// device getAccelerometer appears to do real I²C work, so polling every
	// 30 Hz update is wasted bandwidth — throttle to every 3rd frame
	// (10 Hz). Human shake gestures top out around 5 Hz; this is plenty.
	// At rest |g|² ≈ 1.0; a confident shake pushes it well past that.
	// Cooldown prevents one gesture from firing multiple PM Shake events.
	static int shake_cooldown = 0;
	static int shake_poll_phase = 0;
	if (shake_cooldown > 0) shake_cooldown--;
	if (++shake_poll_phase >= 3) {
		shake_poll_phase = 0;
		float ax, ay, az;
		pd->system->getAccelerometer(&ax, &ay, &az);
		float mag2 = ax * ax + ay * ay + az * az;
		if (mag2 > 2.5f && shake_cooldown == 0) {
			JoystickButtonsEvent(6, 1);
			JoystickButtonsEvent(6, 0);
			shake_cooldown = 2;  // ~200 ms when polling every 3rd frame
		}
	}
}

// 8 PM pixels (1 bit each, MSB = leftmost) expand to 24 Playdate
// framebuffer bits. Precomputed once at startup so each PM byte becomes
// direct byte stores instead of per-pixel read-modify-writes.
static uint8_t expand_lut_3x[256][3];

// Same idea for the 3.5x scale. One PM byte (8 src bits) expands to 28
// dst bits using a 3,4,3,4,3,4,3,4 width pattern per src bit (total
// 8*(3+4)/2 = 28 bits = "3.5 dst bytes" per src byte). Both halves of an
// expand_pair_35x call use the same pattern, so a single 256-entry table
// is enough — packed in the low 28 bits of a uint32_t. ~1 KB rodata.
static uint32_t expand_lut_35x[256];

static void init_expand_lut(void)
{
	for (int i = 0; i < 256; i++) {
		uint32_t bits3 = 0;
		uint32_t bits35 = 0;
		for (int b = 0; b < 8; b++) {
			int width = (b & 1) ? 4 : 3;
			bits35 <<= width;
			if ((i >> (7 - b)) & 1) {
				// MSB-first to match Playdate framebuffer layout.
				bits3 |= (uint32_t)0x7 << (24 - 3 - b * 3);
				bits35 |= (uint32_t)((1u << width) - 1u);
			}
		}
		expand_lut_3x[i][0] = (uint8_t)(bits3 >> 16);
		expand_lut_3x[i][1] = (uint8_t)(bits3 >> 8);
		expand_lut_3x[i][2] = (uint8_t)(bits3);
		expand_lut_35x[i] = bits35;
	}
}

static void expand_pair_35x(uint8_t b0, uint8_t b1, uint8_t *dst)
{
	// Combine two 28-bit halves into a single 56-bit value:
	//   bits 55..28 from b0, bits 27..0 from b1.
	uint32_t e0 = expand_lut_35x[b0];
	uint32_t e1 = expand_lut_35x[b1];

	// Unpack to 7 bytes without touching uint64. e0 lives in bits 27..0
	// of a uint32; in the combined 56-bit output it occupies bits 55..28.
	//   byte 0: bits 55..48 = e0[27..20]
	//   byte 1: bits 47..40 = e0[19..12]
	//   byte 2: bits 39..32 = e0[11..4]
	//   byte 3: bits 31..24 = e0[3..0] (high nibble) | e1[27..24] (low)
	//   byte 4: bits 23..16 = e1[23..16]
	//   byte 5: bits 15..8  = e1[15..8]
	//   byte 6: bits 7..0   = e1[7..0]
	dst[0] = (uint8_t)(e0 >> 20);
	dst[1] = (uint8_t)(e0 >> 12);
	dst[2] = (uint8_t)(e0 >> 4);
	dst[3] = (uint8_t)((e0 << 4) | (e1 >> 24));
	dst[4] = (uint8_t)(e1 >> 16);
	dst[5] = (uint8_t)(e1 >> 8);
	dst[6] = (uint8_t)(e1);
}

// Threshold lookups for the analog render path. MinxLCD_DecayRefresh keeps
// a 4-bit on/off history per pixel in LCDPixelsAS; the original pipeline
// then materialized a smoothed brightness byte into LCDPixelsA which the
// render code thresholded against t_on / t_off. On Playdate, the LCDPixelsA
// store is dead memory traffic in a memory-bound loop (see DecayRefresh
// in source/MinxLCD.c). Instead, we precompute "does this sh value clear
// the high/mid threshold" for the 16 possible sh values once per contrast
// change, then sample LCDPixelsAS[] directly. Same math, ~25% less memory
// traffic in DecayRefresh per PM frame.
static uint8_t pm_sh_to_high[16];
static uint8_t pm_sh_to_mid[16];
static int pm_lut_p0_cached = -1;
static int pm_lut_p1_cached = -1;

static const uint8_t pm_bits_actives_4[16] = {
	0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
};

static void pm_rebuild_threshold_luts(void)
{
	const int p0 = MinxLCD.Pixel0Intensity;
	const int p1 = MinxLCD.Pixel1Intensity;
	const int span = p1 - p0;
	const uint8_t t_off = (uint8_t)(p0 + (3 * span) / 8);
	const uint8_t t_on  = (uint8_t)(p0 + (5 * span) / 8);
	for (int sh = 0; sh < 16; sh++) {
		int level = pm_bits_actives_4[sh];
		int decay = (p0 * (4 - level) + p1 * level) >> 2;
		pm_sh_to_high[sh] = (decay >= t_on)  ? 1 : 0;
		pm_sh_to_mid [sh] = (decay >= t_off) ? 1 : 0;
	}
	pm_lut_p0_cached = p0;
	pm_lut_p1_cached = p1;
}

static inline void pm_ensure_threshold_luts(void)
{
	if (MinxLCD.Pixel0Intensity != pm_lut_p0_cached ||
	    MinxLCD.Pixel1Intensity != pm_lut_p1_cached) {
		pm_rebuild_threshold_luts();
	}
}

// 0 in LCDPixelsD = pixel off = light = white = bit set on Playdate.
//
// PM games fake gray shades by toggling pixels every native frame (72 Hz).
// On a 1-bit panel that's just visible flicker. To suppress it we run the
// emulator in LCDMODE_ANALOG: MinxLCD_DecayRefresh keeps a 4-frame history
// per pixel in LCDPixelsAS, which we threshold via pm_sh_to_high / _mid.
static void build_pm_row_bits(int y, uint8_t row[PM_W / 8])
{
	const int pm_bytes = PM_W / 8;

	if (CommandLine.lcdmode == LCDMODE_2SHADES) {
		const uint8_t *srcD = &LCDPixelsD[y * PM_W];
		for (int bx = 0; bx < pm_bytes; bx++) {
			row[bx] =
				((srcD[0] == 0) << 7) | ((srcD[1] == 0) << 6) |
				((srcD[2] == 0) << 5) | ((srcD[3] == 0) << 4) |
				((srcD[4] == 0) << 3) | ((srcD[5] == 0) << 2) |
				((srcD[6] == 0) << 1) | ((srcD[7] == 0)     );
			srcD += 8;
		}
		return;
	}

	const uint8_t dither = (y & 1) ? 0xAA : 0x55;
	const uint8_t *srcAS = &LCDPixelsAS[y * PM_W];

	for (int bx = 0; bx < pm_bytes; bx++) {
		uint8_t high =
			(pm_sh_to_high[srcAS[0]] << 7) | (pm_sh_to_high[srcAS[1]] << 6) |
			(pm_sh_to_high[srcAS[2]] << 5) | (pm_sh_to_high[srcAS[3]] << 4) |
			(pm_sh_to_high[srcAS[4]] << 3) | (pm_sh_to_high[srcAS[5]] << 2) |
			(pm_sh_to_high[srcAS[6]] << 1) | (pm_sh_to_high[srcAS[7]]     );
		uint8_t mid =
			(pm_sh_to_mid[srcAS[0]] << 7) | (pm_sh_to_mid[srcAS[1]] << 6) |
			(pm_sh_to_mid[srcAS[2]] << 5) | (pm_sh_to_mid[srcAS[3]] << 4) |
			(pm_sh_to_mid[srcAS[4]] << 3) | (pm_sh_to_mid[srcAS[5]] << 2) |
			(pm_sh_to_mid[srcAS[6]] << 1) | (pm_sh_to_mid[srcAS[7]]     );
		srcAS += 8;

		row[bx] = (uint8_t)~(high | (mid & dither));
	}
}

static void render_screen_3x(uint8_t *fb)
{
	const int byte_x = SCALE_3X_X / 8;
	const int pm_bytes = PM_W / 8;

	if (CommandLine.lcdmode == LCDMODE_2SHADES) {
		for (int y = 0; y < PM_H; y++) {
			const uint8_t *srcD = &LCDPixelsD[y * PM_W];
			uint8_t *dst0 = fb + (SCALE_3X_Y + y * 3) * LCD_ROWSIZE + byte_x;
			uint8_t *dst1 = dst0 + LCD_ROWSIZE;
			uint8_t *dst2 = dst1 + LCD_ROWSIZE;

			for (int bx = 0; bx < pm_bytes; bx++) {
				uint8_t b =
					((srcD[0] == 0) << 7) | ((srcD[1] == 0) << 6) |
					((srcD[2] == 0) << 5) | ((srcD[3] == 0) << 4) |
					((srcD[4] == 0) << 3) | ((srcD[5] == 0) << 2) |
					((srcD[6] == 0) << 1) | ((srcD[7] == 0)     );
				srcD += 8;

				const uint8_t *e = expand_lut_3x[b];
				dst0[0] = e[0]; dst0[1] = e[1]; dst0[2] = e[2];
				dst1[0] = e[0]; dst1[1] = e[1]; dst1[2] = e[2];
				dst2[0] = e[0]; dst2[1] = e[1]; dst2[2] = e[2];
				dst0 += 3; dst1 += 3; dst2 += 3;
			}
		}

		pd->graphics->markUpdatedRows(SCALE_3X_Y, SCALE_3X_Y + SCALE_3X_H - 1);
		return;
	}

	for (int y = 0; y < PM_H; y++) {
		const uint8_t *srcAS = &LCDPixelsAS[y * PM_W];
		uint8_t *dst0 = fb + (SCALE_3X_Y + y * 3) * LCD_ROWSIZE + byte_x;
		uint8_t *dst1 = dst0 + LCD_ROWSIZE;
		uint8_t *dst2 = dst1 + LCD_ROWSIZE;
		const uint8_t dither = (y & 1) ? 0xAA : 0x55;

		for (int bx = 0; bx < pm_bytes; bx++) {
			uint8_t high =
				(pm_sh_to_high[srcAS[0]] << 7) | (pm_sh_to_high[srcAS[1]] << 6) |
				(pm_sh_to_high[srcAS[2]] << 5) | (pm_sh_to_high[srcAS[3]] << 4) |
				(pm_sh_to_high[srcAS[4]] << 3) | (pm_sh_to_high[srcAS[5]] << 2) |
				(pm_sh_to_high[srcAS[6]] << 1) | (pm_sh_to_high[srcAS[7]]     );
			uint8_t mid =
				(pm_sh_to_mid[srcAS[0]] << 7) | (pm_sh_to_mid[srcAS[1]] << 6) |
				(pm_sh_to_mid[srcAS[2]] << 5) | (pm_sh_to_mid[srcAS[3]] << 4) |
				(pm_sh_to_mid[srcAS[4]] << 3) | (pm_sh_to_mid[srcAS[5]] << 2) |
				(pm_sh_to_mid[srcAS[6]] << 1) | (pm_sh_to_mid[srcAS[7]]     );
			srcAS += 8;

			const uint8_t b = (uint8_t)~(high | (mid & dither));
			const uint8_t *e = expand_lut_3x[b];
			dst0[0] = e[0]; dst0[1] = e[1]; dst0[2] = e[2];
			dst1[0] = e[0]; dst1[1] = e[1]; dst1[2] = e[2];
			dst2[0] = e[0]; dst2[1] = e[1]; dst2[2] = e[2];
			dst0 += 3; dst1 += 3; dst2 += 3;
		}
	}

	pd->graphics->markUpdatedRows(SCALE_3X_Y, SCALE_3X_Y + SCALE_3X_H - 1);
}

static void render_screen_35x(uint8_t *fb)
{
	const int byte_x = SCALE_35X_X / 8;
	const int pm_bytes = PM_W / 8;
	uint8_t row[PM_W / 8];
	uint8_t scaled_row[SCALE_35X_W / 8];
	int dst_y = SCALE_35X_Y;

	for (int y = 0; y < PM_H; y++) {
		build_pm_row_bits(y, row);
		for (int pair = 0; pair < pm_bytes / 2; pair++) {
			expand_pair_35x(row[pair * 2], row[pair * 2 + 1],
			                &scaled_row[pair * 7]);
		}

		const int repeats = (y & 1) ? 4 : 3;
		for (int r = 0; r < repeats; r++) {
			uint8_t *dst = fb + (dst_y + r) * LCD_ROWSIZE + byte_x;
			memcpy(dst, scaled_row, sizeof(scaled_row));
		}
		dst_y += repeats;
	}

	pd->graphics->markUpdatedRows(SCALE_35X_Y, SCALE_35X_Y + SCALE_35X_H - 1);
}

static void render_screen(void)
{
	if (!LCDDirty) return;
	LCDDirty = 0;

	// Refresh the sh→threshold LUTs if the game adjusted contrast since
	// last render. Cheap check (two int compares); LUT rebuild itself only
	// runs on actual contrast change.
	pm_ensure_threshold_luts();

	uint8_t *fb = pd->graphics->getFrame();
	if (screen_scale_mode == RENDER_SCALE_3X)
		render_screen_3x(fb);
	else
		render_screen_35x(fb);
}

// --- ROM picker -----------------------------------------------------------
//
// Lists *.min files in /Shared/Emulation/pm/games/ (CrankBoy convention) and
// lets the user pick one with D-pad + A. If the directory is empty, falls
// back to the bundled boot.min. Rough/testing UI, not final.

#define MAX_ROMS 128
// 128 covers full English titles and fan-translation names; ROMs that
// exceeded the previous 64-byte cap were silently dropped from the picker.
#define MAX_ROM_NAME 128

static const char *ROM_DIR = "/Shared/Emulation/pm/games/";

static char rom_names[MAX_ROMS][MAX_ROM_NAME];
static int rom_count = 0;
static int rom_cursor = 0;
static int rom_view_top = 0;

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
	size_t flen = strlen(filename);
	pd->system->logToConsole("%s:   listfiles entry: '%s' (len=%u)",
		AppName, filename, (unsigned)flen);
	if (!has_min_suffix(filename)) {
		// Log the trailing bytes so invisible chars (whitespace, BOM) show up
		// as their numeric values rather than disappearing in the console.
		const unsigned char *t = (const unsigned char *)filename;
		size_t off = flen >= 6 ? flen - 6 : 0;
		pd->system->logToConsole(
			"%s:     dropped: suffix mismatch, last bytes: %02X %02X %02X %02X %02X %02X",
			AppName,
			t[off], t[off+1], t[off+2], t[off+3], t[off+4], t[off+5]);
		return;
	}
	if (rom_count >= MAX_ROMS) {
		pd->system->logToConsole("%s:     dropped: MAX_ROMS=%d reached",
			AppName, MAX_ROMS);
		return;
	}
	if (flen >= MAX_ROM_NAME) {
		pd->system->logToConsole("%s:     dropped: name length %u >= MAX_ROM_NAME=%d",
			AppName, (unsigned)flen, MAX_ROM_NAME);
		return;
	}
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
	MinxAudio_ChangeEngine(CommandLine.sound);
#ifdef PD_OPCODE_DIAG
	MinxCPU_OpcodeDiagReset();
#endif

	// Repaint the whole framebuffer black so the border around the PM screen
	// stays black without per-frame redraws (and overwrites the picker UI).
	uint8_t *fb = pd->graphics->getFrame();
	memset(fb, 0x00, (size_t)(LCD_ROWSIZE * LCD_ROWS));
	pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);

	LCDDirty = 1;
	app_mode = MODE_EMULATOR;
	start_audio_source();
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
	stop_audio_source();
}

static void menu_item_picker_cb(void *userdata)
{
	(void)userdata;
	return_to_picker();
}

static void menu_item_lcd_mode_cb(void *userdata)
{
	(void)userdata;
	if (!lcd_mode_menu_item) return;

	int fast = pd->system->getMenuItemValue(lcd_mode_menu_item);
	int new_mode = fast ? LCDMODE_2SHADES : LCDMODE_ANALOG;
	if (CommandLine.lcdmode == new_mode) return;

	CommandLine.lcdmode = new_mode;
	PokeMini_ApplyChanges();
	LCDDirty = MINX_DIRTYSCR;
}

static void menu_item_screen_scale_cb(void *userdata)
{
	(void)userdata;
	if (!screen_scale_menu_item) return;

	int new_scale = pd->system->getMenuItemValue(screen_scale_menu_item)
		? RENDER_SCALE_35X : RENDER_SCALE_3X;
	if (screen_scale_mode == new_scale) return;

	screen_scale_mode = new_scale;
	uint8_t *fb = pd->graphics->getFrame();
	memset(fb, 0x00, (size_t)(LCD_ROWSIZE * LCD_ROWS));
	pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
	LCDDirty = MINX_DIRTYSCR;
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

#ifdef PD_PERF_DIAG
	unsigned int update_start = PDPerf_NowUs();
#endif

	static int frame_accum = 0;
	frame_accum += 12;
	int pm_frames = frame_accum / 5;
	frame_accum -= pm_frames * 5;

	// PERF KEEPALIVE (do NOT strip — see NOTES.md "Perf-keepalive
	// minimal recipe" entry). Empirically required to keep on-device
	// throughput at ~25 fps; without it, perf drops to ~12 fps on heavy
	// ROMs even though the elf is byte-identical in the hot path.
	//
	// Minimum recipe found by bisection:
	//   - 4× pd->system->getElapsedTime() per update (returns are
	//     discarded; the calls themselves are what matters)
	//   - one format-rich pd->system->logToConsole() per second, with
	//     %f-style float arg(s) so the firmware does FP format work
	//
	// Smaller variants (no syscalls / 1× syscall / log alone) cap at
	// ~22 fps. Adding per-update FP math is negative (-2 fps overhead).
	// We don't fully understand the firmware-side mechanism — see NOTES.
	{
		(void)pd->system->getElapsedTime();
		(void)pd->system->getElapsedTime();
		(void)pd->system->getElapsedTime();
		(void)pd->system->getElapsedTime();

		static int   keepalive_count = 0;
		static int   keepalive_total = 0;
		static unsigned int keepalive_t0 = 0;
		keepalive_total++;
		if (++keepalive_count >= 30) {
			keepalive_count = 0;
			unsigned int now = pd->system->getCurrentTimeMilliseconds();
			unsigned int dt  = (keepalive_t0 == 0) ? 0 : now - keepalive_t0;
			keepalive_t0 = now;
			static float keepalive_f = 1.5f;
			keepalive_f *= 1.0001f;
			if (keepalive_f > 1.0e9f) keepalive_f = 1.5f;
			pd->system->logToConsole(
				"perf: total=%d dt=%ums (rate=%.1ffps) f=%.3f",
				keepalive_total, dt,
				dt > 0 ? 30000.0f / dt : 0.0f, keepalive_f);
		}
	}

#ifdef PD_PERF_DIAG
	unsigned int t0 = PDPerf_NowUs();
#endif
	for (int i = 0; i < pm_frames; i++) PokeMini_EmulateFrame();
#ifdef PD_PERF_DIAG
	pdperf_emu_us += PDPerf_NowUs() - t0;
	pdperf_pm_frames += (unsigned int)pm_frames;
	t0 = PDPerf_NowUs();
#endif

	handle_input();
#ifdef PD_PERF_DIAG
	pdperf_input_us += PDPerf_NowUs() - t0;
	t0 = PDPerf_NowUs();
#endif
	render_screen();
#ifdef PD_PERF_DIAG
	pdperf_render_us += PDPerf_NowUs() - t0;
	pdperf_update_us += PDPerf_NowUs() - update_start;
	pdperf_updates++;
	if (pdperf_updates >= 30) pdperf_log_and_reset();
#endif
#ifdef PD_OPCODE_DIAG
	static unsigned int opdiag_updates = 0;
	if (++opdiag_updates >= PD_OPDIAG_LOG_UPDATES) {
		pdopdiag_log_and_reset(opdiag_updates);
		opdiag_updates = 0;
	}
#endif

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
		// 512 is the empirical ceiling on this hardware. Tried 1024 and 2048;
		// both break PRC frame-render timing (LCDPixelsA never populates →
		// white PM screen, audio plays normally because it runs on a
		// separate thread). Don't push this past 512. See NOTES.md.
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

		// The audio source is started when a ROM starts, not while the picker
		// is up. Creating it at the emulation transition avoids occasional
		// silent-source behavior observed on device after layout-only rebuilds.

		// System menu item: lets the player return to the ROM picker without
		// quitting the app. Persists for the lifetime of the process.
		pd->system->addMenuItem("ROM Picker", menu_item_picker_cb, NULL);
		lcd_mode_menu_item = pd->system->addOptionsMenuItem(
			"LCD Mode", lcd_mode_options, 2, menu_item_lcd_mode_cb, NULL);
		if (lcd_mode_menu_item) {
			// Soft is the default: analog decay suppresses Pokemon Mini LCD
			// flicker. Fast is available for performance measurements.
			pd->system->setMenuItemValue(lcd_mode_menu_item, 0);
		}
		screen_scale_menu_item = pd->system->addOptionsMenuItem(
			"Scale", screen_scale_options, 2, menu_item_screen_scale_cb, NULL);
		if (screen_scale_menu_item) {
			// 3x is the default stable integer-scale mode.
			pd->system->setMenuItemValue(screen_scale_menu_item, 0);
		}

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

	} else if (event == kEventPause) {
		emu_paused = 1;
		// Release every PM key so no button stays stuck when the player returns.
		// The Playdate runtime stops calling update() while the menu is up, so
		// held-button state from handle_input() would otherwise persist forever.
		for (int k = MINX_KEY_A; k <= MINX_KEY_SHOCK; k++)
			PokeMini_KeypadEvent((uint8_t)k, 0);

	} else if (event == kEventResume) {
		emu_paused = 0;
		if (app_mode == MODE_EMULATOR)
			pd->sound->setOutputsActive(1, 1);

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
