/*
  PokeMini - Pokemon-Mini Emulator
  Playdate port — EEPROM persistence helpers

  These helpers are in a separate translation unit listed AFTER Hardware.c in
  GAME_SOURCES. That ordering keeps them after PokeMini_EmulateFrame in the
  linked binary, preventing them from shifting EmulateFrame into I-cache set 19
  (which is permanently occupied by MinxTimers_Sync). See NOTES.md
  "EEPROM save/load and I-cache aliasing" for the full analysis.
*/

#include "pd_api.h"
#include <stdlib.h>
#include <string.h>

#include "PokeMini.h"  // pulls in PMCommon.h (PMTMPV), CommandLine.h, MinxIO.h, etc.

// Defined in PokeMini_Playdate.c
extern PlaydateAPI *pd;
extern const char  *AppName;

void ensure_eep_dir(void)
{
	pd->file->mkdir("/Shared");
	pd->file->mkdir("/Shared/Emulation");
	pd->file->mkdir("/Shared/Emulation/pm");
	pd->file->mkdir("/Shared/Emulation/pm/saves");
}

void setup_eeprom_path(const char *rom_path)
{
	if (!rom_path) {
		CommandLine.eeprom_file[0] = 0;
		return;
	}
	const char *base = strrchr(rom_path, '/');
	base = base ? base + 1 : rom_path;

	char name[128];
	strncpy(name, base, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	char *dot = strrchr(name, '.');
	if (dot) *dot = 0;

	snprintf(CommandLine.eeprom_file, sizeof(CommandLine.eeprom_file),
	         "/Shared/Emulation/pm/saves/%s.eep", name);
}

int pd_load_eeprom(const char *filename)
{
	SDFile *f = pd->file->open(filename, kFileRead | kFileReadData);
	if (!f) return 0;

	int bytes = pd->file->read(f, EEPROM, 8192);
	pd->file->close(f);
	pd->system->logToConsole("%s: EEPROM loaded (%d bytes): %s",
	    AppName, bytes, filename);
	return (bytes == 8192) ? 1 : 0;
}

int pd_save_eeprom(const char *filename)
{
	// Create saves dir lazily — not at kEventInit — so no flash I/O runs
	// during the Playdate firmware's startup window (see NOTES.md).
	ensure_eep_dir();
	SDFile *f = pd->file->open(filename, kFileWrite);
	if (!f) {
		pd->system->logToConsole("%s: EEPROM save failed (open): %s",
		    AppName, filename);
		return 0;
	}
	int bytes = pd->file->write(f, EEPROM, 8192);
	pd->file->close(f);
	pd->system->logToConsole("%s: EEPROM saved (%d bytes): %s",
	    AppName, bytes, filename);
	return (bytes == 8192) ? 1 : 0;
}
