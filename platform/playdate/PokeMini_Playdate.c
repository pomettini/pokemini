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

// Blit the PM 96x64 frame into the Playdate 400x240 1-bit framebuffer at 3x
// integer scale, centered at (SCREEN_X, SCREEN_Y).
// We read LCDPixelsD directly: 0 = pixel off (light background) = white on
// Playdate; non-zero = pixel on (dark ink) = black on Playdate.
// This avoids any palette index ambiguity (Pixel0Intensity is ~240, not 0).
static void render_screen(void)
{
	LCDDirty = 0;

	uint8_t *fb = pd->graphics->getFrame();

	for (int y = 0; y < PM_H; y++) {
		for (int x = 0; x < PM_W; x++) {
			// Off pixel = light background = white; on pixel = dark ink = black
			int white = (LCDPixelsD[y * PM_W + x] == 0);
			for (int dy = 0; dy < PM_SCALE; dy++) {
				uint8_t *row = fb + (SCREEN_Y + y * PM_SCALE + dy) * LCD_ROWSIZE;
				for (int dx = 0; dx < PM_SCALE; dx++) {
					int px = SCREEN_X + x * PM_SCALE + dx;
					uint8_t mask = (uint8_t)(0x80 >> (px & 7));
					if (white) row[px >> 3] |=  mask;
					else       row[px >> 3] &= ~mask;
				}
			}
		}
	}

	pd->graphics->markUpdatedRows(SCREEN_Y, SCREEN_Y + PM_H * PM_SCALE - 1);
}

// Step the emulator by a small number of cycles, just enough to retire one
// instruction worth of work plus PRC/timer sync.  Mirrors the inner loop of
// PokeMini_EmulateFrame but allows the caller to log PC between steps.
static int step_one_instruction(void)
{
	int cycles = 0;
	if (StallCPU) {
		cycles = StallCycles;
		PokeHWCycles = cycles;
	} else {
		cycles = MinxCPU_Exec();
		PokeHWCycles = cycles;
	}
	MinxTimers_Sync();
	MinxPRC_Sync();
	return cycles;
}

// Update callback: called by the Playdate runtime at 30 fps.
// Emulate 2 PM frames per display frame so the emulated rate (~60 Hz) stays
// close to the Pokemon Mini's native 72 Hz.
static int update(void *userdata)
{
	(void)userdata;

	static int diag_frame = 0;

	// On the very first update, dump a step-by-step PC trace of what the CPU
	// does before reaching the eventual stuck state.  We log the first N
	// instructions so we can see the boot sequence on real hardware.
	if (diag_frame == 0) {
		pd->system->logToConsole("%s: BIOS[0..15]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			AppName,
			PM_BIOS[0], PM_BIOS[1], PM_BIOS[2], PM_BIOS[3],
			PM_BIOS[4], PM_BIOS[5], PM_BIOS[6], PM_BIOS[7],
			PM_BIOS[8], PM_BIOS[9], PM_BIOS[10], PM_BIOS[11],
			PM_BIOS[12], PM_BIOS[13], PM_BIOS[14], PM_BIOS[15]);
		pd->system->logToConsole("%s: pre-emul V=%02X PC=%04X SP=%04X F=%02X U1=%02X U2=%02X ShU=%d",
			AppName,
			(int)MinxCPU.PC.B.I, (int)MinxCPU.PC.W.L,
			(int)MinxCPU.SP.W.L, (int)MinxCPU.F,
			(int)MinxCPU.U1, (int)MinxCPU.U2, (int)MinxCPU.Shift_U);

		// Trace first 64 instructions
		for (int n = 0; n < 64; n++) {
			pd->system->logToConsole(
				"%s: t=%d V=%02X PC=%04X IR=%02X SP=%04X F=%02X PRCm=%d ACT=%02X Dirty=%d",
				AppName, n,
				(int)MinxCPU.PC.B.I, (int)MinxCPU.PC.W.L,
				(int)MinxCPU.IR, (int)MinxCPU.SP.W.L,
				(int)MinxCPU.F, (int)MinxPRC.PRCMode,
				(int)PMR_IRQ_ACT1, LCDDirty);
			step_one_instruction();
		}
	}

	PokeMini_EmulateFrame();
	PokeMini_EmulateFrame();

	handle_input();

	// Log first 60 frames to diagnose device rendering issues.
	if (diag_frame < 60) {
		int on = 0;
		for (int i = 0; i < PM_W * PM_H; i++) if (LCDPixelsD[i]) on++;
		pd->system->logToConsole(
			"%s: f=%d V=%02X PC=%04X F=%02X Stat=%d MIrq=%d ENA=%02X ACT=%02X PRC=%d Disp=%d Dirty=%d on=%d",
			AppName, diag_frame,
			(int)MinxCPU.PC.B.I,
			(int)MinxCPU.PC.W.L,
			(int)MinxCPU.F,
			(int)MinxCPU.Status,
			MinxIRQ_MasterIRQ,
			(int)PMR_IRQ_ENA1,
			(int)PMR_IRQ_ACT1,
			(int)MinxPRC.PRCMode,
			(int)MinxLCD.DisplayOn,
			LCDDirty, on);
		diag_frame++;
	}

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

		// Configure emulator defaults suitable for Playdate
		CommandLineInit();
		CommandLine.sound      = 1;
		CommandLine.lcdfilter  = 0;
		CommandLine.lcdmode    = LCDMODE_2SHADES;
		CommandLine.synccycles = 8;

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
