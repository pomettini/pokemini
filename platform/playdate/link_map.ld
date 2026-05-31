/* Custom linker script for the Playdate device build.
 *
 * Identical to the SDK's link_map.ld except that .text.hot (functions
 * tagged with POKEMINI_HOT) goes first in .text. The intent is to keep
 * the PM CPU dispatcher and its tightest helpers contiguous and at the
 * lowest .text addresses, so they can share cache lines and stay
 * resident in the M7 Rev A's tiny 4 KB I-cache for as much of the hot
 * loop as possible.
 *
 * Keep in sync with the SDK's link_map.ld if Panic ever updates it.
 */

ENTRY(eventHandlerShim)
GROUP(libgcc.a libc.a libm.a)

SECTIONS
{
	.text :
	{
		/* Hot path: PM dispatcher + frequently-called helpers.
		 *
		 * .text.hot.exec (just MinxCPU_Exec) is forced to a 0x1000-byte
		 * boundary first, to manage branch-predictor aliasing — the
		 * Cortex-M7 predictor table is keyed on bits mod 0x1000, and
		 * putting the single hottest function at a known offset within
		 * its page makes it less likely to alias with other hot code.
		 * Trick borrowed from CrankBoyHQ/crankboy-app's link_map.ld.
		 *
		 * Other .text.hot functions follow contiguously after a cache-
		 * line boundary.
		 */
		. = ALIGN(0x1000);
		. += 0x140;
		*(.text.hot.exec)
		. = ALIGN(32);
		*(.text.hot*)
		. = ALIGN(32);

		/* Pin PokeMini_EmulateFrame (tagged POKEMINI_EMULATE_FRAME) to I-cache
		 * set 1. The set index is bits[9:5] of the address, so +0x20 from a
		 * 0x400-aligned boundary lands the function start in set 1, clear of
		 * MinxTimers_Sync at set 19. Confirmed by 2026-05-15 diagnostic: without
		 * this pinning, EmulateFrame ends up at set 16 and average fps drops
		 * ~2-3 fps. See NOTES.md "EEPROM save/load and I-cache aliasing". */
		. = ALIGN(0x400) + 0x20;
		*(.text.emulate_frame)
		. = ALIGN(32);

		/* Drop in non-Playdate-port code first so its size shrinks the padding
		 * we need before update(). */
		*(.text)
		*(EXCLUDE_FILE(*PokeMini_Playdate.c.obj) .text.*)

		/* Pin .text.update at mod 0x400 = 0x034 so the per-frame loop's blt.w
		 * and BL-to-EmulateFrame land in the same branch-predictor slots as
		 * the no-EEPROM baseline build. Measured 2026-05-15: this pin saves
		 * ~1 fps at title screen (1146ms → 1093ms). See NOTES.md "EEPROM via
		 * callback swap". */
		. = ALIGN(0x400) + 0x34;
		*(.text.update)
		. = ALIGN(32);

		/* Remaining PokeMini_Playdate.c sections (cold paths). */
		*(.text.*)

		/* ITCM-style relocatable callbacks. Grouped contiguously and
		 * bracketed by __itcm_pm_start/end markers so PokeMini_Playdate.c
		 * update() can memcpy the whole region to a stack buffer. The
		 * code in .itcm_pm must be relocatable: no PC-relative BL to
		 * external functions (use function pointers for those instead),
		 * compiled with -fno-jump-tables. See PokeMini_ITCMCallbacks.c. */
		. = ALIGN(32);
		__itcm_pm_start = .;
		*(.itcm_pm)
		*(.itcm_pm.*)
		. = ALIGN(32);
		__itcm_pm_end = .;

		KEEP(*(.init))
		KEEP(*(.fini))

		/* .ctors */
		*crtbegin.o(.ctors)
		*crtbegin?.o(.ctors)
		*(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
		*(SORT(.ctors.*))
		*(.ctors)

		/* .dtors */
		*crtbegin.o(.dtors)
		*crtbegin?.o(.dtors)
		*(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
		*(SORT(.dtors.*))
		*(.dtors)

		*(.rodata*)

		KEEP(*(.eh_frame*))

	}

	.data :
	{
		__etext = .;

		__data_start__ = .;
		*(vtable)
		*(.data*)

		. = ALIGN(4);
		/* preinit data */
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP(*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);

		. = ALIGN(4);
		/* init data */
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP(*(SORT(.init_array.*)))
		KEEP(*(.init_array))
		PROVIDE_HIDDEN (__init_array_end = .);

		. = ALIGN(4);
		/* finit data */
		PROVIDE_HIDDEN (__fini_array_start = .);
		KEEP(*(SORT(.fini_array.*)))
		KEEP(*(.fini_array))
		PROVIDE_HIDDEN (__fini_array_end = .);

		. = ALIGN(4);
		/* All data end */
		__data_end__ = .;

	}

	.bss :
	{
		. = ALIGN(4);
		__bss_start__ = .;
		*(.bss*)
		*(COMMON)
		*(COM)
		. = ALIGN(4);
		__bss_end__ = .;

	}

  /DISCARD/ :
  {
		*(.ARM.exidx)
  }

}
