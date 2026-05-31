/*
  PokeMini Playdate port — separate user stack for emulation.

  Ported from CrankBoyHQ/crankboy-app's src/userstack.c.

  The Playdate firmware places the M7's stack at the top of DTCM. If we
  want to use the bottom of DTCM for our own data (PM_RAM, hot state),
  we need to keep the actual stack OUT of DTCM so the firmware stack and
  our DTCM allocations don't collide.

  Solution: a fixed `user_stack[16 KB]` array in `.bss` (SDRAM). We switch
  SP to point at user_stack before running emulation, and switch back
  when done. While emulation runs, all stack accesses go to SDRAM (not
  DTCM), leaving the DTCM region we allocated free of stack writes.

  The switch is done via inline ARM assembly in PokeMini_UserStack.c.
*/

#ifndef POKEMINI_USERSTACK_H
#define POKEMINI_USERSTACK_H

#include <stdint.h>

#ifdef TARGET_PLAYDATE

typedef void *(*user_stack_fn)(void *);

void init_user_stack(void);
void validate_user_stack(void);

/* Run fn(arg) on the dedicated user_stack. */
void *call_with_user_stack_1(user_stack_fn fn, void *arg);

#else

#define call_with_user_stack_1(fn, a) ((fn)(a))
static inline void init_user_stack(void) {}
static inline void validate_user_stack(void) {}

#endif

#endif
