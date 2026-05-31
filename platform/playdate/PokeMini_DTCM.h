/*
  PokeMini Playdate port — runtime DTCM allocator.

  Borrowed/simplified from CrankBoyHQ/crankboy-app's src/dtcm.c. The
  trick: the Playdate firmware places the user stack at the top of the
  M7's DTCM region, growing down. The SDK reserves __STACK_SIZE bytes
  (default 60 KB) but actual call-chain usage is typically much less.
  Anything BELOW the stack-bottom is reserved-but-unused DTCM.

  At startup (kEventInit), compute the boundary:
      dtcm_mempool = __builtin_frame_address(0) - POKEMINI_DTCM_STACK_RESERVE
  This is the highest address we promise the stack will NOT grow past
  (with margin). Below it = free DTCM for app use.

  The reserve value (0x2710 = 10 KB) is the same conservative number
  CrankBoy uses; their comment says it was chosen empirically to avoid
  Rev A crashes. PokeMini's call chain is unlikely to exceed this.

  This is a tiny bump-allocator. No free() — we only need long-lived hot
  buffers.
*/

#ifndef POKEMINI_DTCM_H
#define POKEMINI_DTCM_H

#include <stdint.h>
#include <stddef.h>

/* Matches CrankBoy's empirically-tuned value. Works because we now run
 * the emulator on user_stack (SDRAM), so the firmware DTCM stack stays
 * shallow during emulation and the 10 KB reserve is plenty. */
#define POKEMINI_DTCM_STACK_RESERVE 0x2710  /* 10 KB */

/* Call once at kEventInit (or as early as possible — the higher the
 * frame pointer, the more DTCM headroom). */
void dtcm_init(void *frame_ptr_at_startup);

/* Returns NULL if DTCM is not initialized or out of room. */
void *dtcm_alloc(size_t size);
void *dtcm_alloc_aligned(size_t size, size_t alignment);

/* Returns the DTCM base address that's currently free. For logging. */
void *dtcm_get_mempool(void);

#endif
