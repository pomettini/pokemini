/*
  PokeMini Playdate port — relocatable memory-access callbacks for the
  ITCM-stack-copy experiment.

  Pattern from Beetle VB / Red Viper: build a small fast-path version of
  MinxCPU_OnRead / MinxCPU_OnWrite that is *relocatable* (no PC-relative
  BL to external functions), place it in its own section, and memcpy that
  section into a stack buffer at frame start. The dispatcher calls the
  relocated copy via a function pointer, so the body executes from the
  stack (in DTCM if the Playdate stack lives in DTCM).

  Why this can be relocated where the original MinxCPU_OnRead can't:
  - The original MinxCPU_OnRead has PC-relative BL instructions to
    MinxIO_ReadReg / MinxTimers_ReadReg / etc. Those offsets would be
    wrong after memcpy.
  - This relocatable version inlines the hot regions (ROM, RAM, BIOS)
    and routes the I/O fallback through a function POINTER (BLX register,
    indirect — has no PC-relative offset that breaks under relocation).
  - Global-variable accesses (PM_ROM, PM_RAM, PM_BIOS, PM_ROM_Mask) compile
    to PC-relative literal loads. The literal pool moves *with* the code
    when memcpy'd, and the literals hold absolute addresses, so those
    stay correct.

  This file is compiled with -fno-jump-tables (set in CMakeLists.txt) so
  the compiler doesn't introduce a PC-relative jump table either.

  See platform/playdate/NOTES.md "ITCM hot-callback copy".
*/

#include <stdint.h>
#include "PokeMini.h"
#include "MinxCPU.h"

#if defined(TARGET_PLAYDATE) && POKEMINI_ITCM_CALLBACKS

extern uint8_t MinxCPU_OnRead(int cpu, uint32_t addr);
extern void MinxCPU_OnWrite(int cpu, uint32_t addr, uint8_t data);

// Function pointers used by the relocatable read/write for I/O fallback.
// Pointed at the real (non-relocatable) MinxCPU_OnRead/OnWrite so the
// uncommon-path I/O register reads go through their normal switch.
uint8_t (*g_pm_io_read)(int, uint32_t) = MinxCPU_OnRead;
void (*g_pm_io_write)(int, uint32_t, uint8_t) = MinxCPU_OnWrite;

// The relocatable read. Marked with the .itcm_pm section attribute so
// the linker groups it adjacent to MinxCPU_ITCMWrite; update() will
// memcpy the pair into a stack buffer and call them from there.
__attribute__((section(".itcm_pm")))
uint8_t MinxCPU_ITCMRead(int cpu, uint32_t addr)
{
	if (addr >= 0x2100) {
		if (PM_ROM) return PM_ROM[addr & PM_ROM_Mask];
		return 0xFF;
	}
	if (addr >= 0x2000) {
		return g_pm_io_read(cpu, addr);  // I/O via fn ptr — safe under relocation
	}
	if (addr >= 0x1000) {
		return PM_RAM[addr - 0x1000];
	}
	return PM_BIOS[addr];
}

__attribute__((section(".itcm_pm")))
void MinxCPU_ITCMWrite(int cpu, uint32_t addr, uint8_t data)
{
	if (addr >= 0x2100) {
		return;  // ROM is read-only
	}
	if (addr >= 0x2000) {
		g_pm_io_write(cpu, addr, data);
		return;
	}
	if (addr >= 0x1000) {
		PM_RAM[addr - 0x1000] = data;
		return;
	}
	// BIOS is read-only; do nothing.
}

// Hot-path function pointers used by MINXCPU_READ/WRITE under
// POKEMINI_ITCM_CALLBACKS. Initialized to point at the in-section
// originals; per-frame relocation re-points them at the stack copies.
uint8_t (*g_pm_read)(int, uint32_t) = MinxCPU_ITCMRead;
void (*g_pm_write)(int, uint32_t, uint8_t) = MinxCPU_ITCMWrite;

#endif
