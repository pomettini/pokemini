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
		 * set 1, and keep the BL-to-EmulateFrame in update() at the same
		 * branch-predictor slot (mod 0x400) as the baseline build.
		 *
		 * Hot section ends at ~0x4a00. ALIGN(0x400) → 0x4c00. +0x20 → 0x4c20
		 * (set 1). libgcc shift = 0x4c20 + EmulateFrame(~0x100) − 0x4a00 = 0x320.
		 *
		 * Total shift = code_growth_before_update + libgcc_shift must be a
		 * multiple of 0x400 so update lands at the same BTB slot as baseline
		 * (mod 0x400 = 0x034). Target: 0x0E0 + 0x320 = 0x400. See NOTES.md
		 * "Branch predictor slot engineering" for the full derivation. */
		. = ALIGN(0x400) + 0x20;
		*(.text.emulate_frame)
		. = ALIGN(32);

		*(.text)
		*(.text.*)

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
