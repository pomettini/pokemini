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
	"Crank",  //  6
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
	 6,  // Shake -> Crank
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

// Load a .min ROM from the Playdate data directory into a heap buffer, then hand
// the buffer to the emulator via PokeMini_SetMINMem.  We use pd->file->* here
// because the standard fopen() is not available on Playdate hardware.
static int load_rom(const char *path)
{
	SDFile *f = pd->file->open(path, kFileRead);
	if (!f) return 0;

	pd->file->seek(f, 0, SEEK_END);
	int size = pd->file->tell(f);
	pd->file->seek(f, 0, SEEK_SET);

	if (size <= 0) {
		pd->file->close(f);
		return 0;
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
// A significant crank movement is treated as the PM "Shake" input.
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

	// Treat a quarter-turn or more of crank movement as a "Shake" pulse
	float delta = pd->system->getCrankChange();
	if (delta > 45.0f || delta < -45.0f) {
		JoystickButtonsEvent(6, 1);
		JoystickButtonsEvent(6, 0);
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
// emulator in LCDMODE_3SHADES (which copies the previous PRC frame's pixels
// into LCDPixelsA) and classify each pixel into 3 buckets:
//   - on in both frames  -> always show on
//   - off in both frames -> always show off
//   - flickering         -> show stable checkerboard (perceived as gray)
// The checkerboard alternates per row so the spatial pattern is even.
static void render_screen(void)
{
	if (!LCDDirty) return;
	LCDDirty = 0;

	uint8_t *fb = pd->graphics->getFrame();
	const int byte_x = SCREEN_X / 8;  // 7
	const int pm_bytes = PM_W / 8;    // 12

	for (int y = 0; y < PM_H; y++) {
		const uint8_t *srcD = &LCDPixelsD[y * PM_W];
		const uint8_t *srcA = &LCDPixelsA[y * PM_W];
		uint8_t *dst0 = fb + (SCREEN_Y + y * PM_SCALE)     * LCD_ROWSIZE + byte_x;
		uint8_t *dst1 = dst0 + LCD_ROWSIZE;
		uint8_t *dst2 = dst1 + LCD_ROWSIZE;

		// Per-row dither mask: bit set means "this column lights up when the
		// pixel is flickering." Alternates between rows for a checkerboard.
		const uint8_t dither = (y & 1) ? 0xAA : 0x55;

		for (int bx = 0; bx < pm_bytes; bx++) {
			// "both" byte: pixel on in current AND previous frame (steady on).
			uint8_t both =
				((srcD[0] & srcA[0]) << 7) | ((srcD[1] & srcA[1]) << 6) |
				((srcD[2] & srcA[2]) << 5) | ((srcD[3] & srcA[3]) << 4) |
				((srcD[4] & srcA[4]) << 3) | ((srcD[5] & srcA[5]) << 2) |
				((srcD[6] & srcA[6]) << 1) | ((srcD[7] & srcA[7])     );
			// "either" byte: pixel on in current OR previous frame.
			uint8_t either =
				((srcD[0] | srcA[0]) << 7) | ((srcD[1] | srcA[1]) << 6) |
				((srcD[2] | srcA[2]) << 5) | ((srcD[3] | srcA[3]) << 4) |
				((srcD[4] | srcA[4]) << 3) | ((srcD[5] | srcA[5]) << 2) |
				((srcD[6] | srcA[6]) << 1) | ((srcD[7] | srcA[7])     );
			srcD += 8; srcA += 8;

			// On = always-on pixels, plus flickering pixels gated by dither.
			// Invert to get "off" mask (Playdate: bit set = white = pixel off).
			uint8_t b = (uint8_t)~(both | (either & dither));

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

		init_expand_lut();

		// Configure emulator defaults suitable for Playdate
		CommandLineInit();
		CommandLine.sound      = 1;
		CommandLine.lcdfilter  = 0;
		// LCDMODE_3SHADES so the emulator core captures the previous PRC frame
		// into LCDPixelsA (Hardware.c:333). render_screen ORs current+previous
		// to suppress the flicker PM games use to fake gray on a 1-bit panel.
		CommandLine.lcdmode    = LCDMODE_3SHADES;
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

		// Load game.min from the Playdate data directory (place it in Source/)
		if (!load_rom("game.min")) {
			pd->system->logToConsole("%s: game.min not found in data directory", AppName);
		} else {
			pd->system->logToConsole("%s: ROM loaded OK, size=%d mask=0x%X", AppName, PM_ROM_Size, PM_ROM_Mask);
			pd->system->logToConsole("%s: ROM[0x2100..05]: %02X %02X %02X %02X %02X %02X",
				AppName,
				PM_ROM[0x2100 & PM_ROM_Mask],
				PM_ROM[0x2101 & PM_ROM_Mask],
				PM_ROM[0x2102 & PM_ROM_Mask],
				PM_ROM[0x2103 & PM_ROM_Mask],
				PM_ROM[0x2104 & PM_ROM_Mask],
				PM_ROM[0x2105 & PM_ROM_Mask]);
		}

		// Soft reset matches what PokeMini_LoadFromCommandLines does in other ports
		// (the hard-reset BIOS path initializes SYS_CTRL3 differently, which can
		// leave the game stuck before it enables the PRC 72Hz interrupt).
		pd->system->logToConsole("%s: pre-Reset", AppName);
		PokeMini_Reset(0);
		pd->system->logToConsole("%s: post-Reset V=%02X PC=%04X SP=%04X F=%02X",
			AppName,
			(int)MinxCPU.PC.B.I, (int)MinxCPU.PC.W.L,
			(int)MinxCPU.SP.W.L, (int)MinxCPU.F);

		// Dump the 16 bytes of RAM at the top of the stack (1FF0..1FFF).
		// PokeMini_Create memsets RAM to 0xFF; if the BIOS hasn't pushed
		// anything before the cart's first far-RET, those 0xFF bytes get
		// popped into V:PC and the CPU jumps to 0xFFFFFF.
		pd->system->logToConsole(
			"%s: stack[1FF0..1FFF]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			AppName,
			PM_RAM[0x0FF0], PM_RAM[0x0FF1], PM_RAM[0x0FF2], PM_RAM[0x0FF3],
			PM_RAM[0x0FF4], PM_RAM[0x0FF5], PM_RAM[0x0FF6], PM_RAM[0x0FF7],
			PM_RAM[0x0FF8], PM_RAM[0x0FF9], PM_RAM[0x0FFA], PM_RAM[0x0FFB],
			PM_RAM[0x0FFC], PM_RAM[0x0FFD], PM_RAM[0x0FFE], PM_RAM[0x0FFF]);

		PokeMini_ApplyChanges();
		pd->system->logToConsole("%s: post-ApplyChanges", AppName);

		UIMenu_Init();
		pd->system->logToConsole("%s: post-UIMenu_Init", AppName);

		// Paint the entire display black so the border around the PM screen
		// stays black without needing to be redrawn each frame
		uint8_t *fb = pd->graphics->getFrame();
		memset(fb, 0x00, (size_t)(LCD_ROWSIZE * LCD_ROWS));
		pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
		pd->system->logToConsole("%s: post-fb-clear", AppName);

		// Start continuous audio output
		audio_source = pd->sound->addSource(audio_callback, NULL, 0);
		pd->system->logToConsole("%s: post-addSource src=%p", AppName, (void *)audio_source);

		pd->system->setUpdateCallback(update, pd);
		pd->system->logToConsole("%s: post-setUpdateCallback (init done)", AppName);

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
	}

	return 0;
}
