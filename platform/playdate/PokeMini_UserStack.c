/*
  Separate user stack for emulation. See PokeMini_UserStack.h.

  Implementation ported and simplified from CrankBoyHQ/crankboy-app's
  src/userstack.c. We only need the 1-argument variant (`update()` takes
  a single `void *userdata`).
*/

#include "PokeMini_UserStack.h"

#ifdef TARGET_PLAYDATE

#include <stdint.h>

#define USER_STACK_SIZE 0x4000  /* 16 KB, same as CrankBoy */
#define CANARY_VALUE   0x5AC3FA3Bu

/* The user stack lives in .bss (SDRAM). Aligned to 8 bytes per AAPCS. */
static char user_stack[USER_STACK_SIZE] __attribute__((aligned(8)));

/* Saved SP from the calling context (firmware stack), used by
 * call_with_main_stack_impl if we ever add it. Declared globally so the
 * naked-asm function can reference it by symbol. */
void *user_stack_exit_sp;

static inline uint32_t *get_stack_start_canary(void)
{
	return (uint32_t *)(user_stack);
}

static inline uint32_t *get_stack_end_canary(void)
{
	return (uint32_t *)(user_stack + USER_STACK_SIZE - sizeof(uint32_t));
}

void init_user_stack(void)
{
	*get_stack_start_canary() = CANARY_VALUE;
	*get_stack_end_canary()   = CANARY_VALUE;
}

void validate_user_stack(void)
{
	extern void *pd;  /* PlaydateAPI*, declared in PokeMini_Playdate.c */
	(void)pd;
	if (*get_stack_start_canary() != CANARY_VALUE ||
	    *get_stack_end_canary()   != CANARY_VALUE) {
		// pd->system->error would be cleaner but adds an include dep cycle.
		// On canary corruption, fall through — the next firmware access
		// will likely panic with a clearer error.
		__asm__ volatile("bkpt #0");
	}
}

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

/* Call fn(arg) on the user_stack. Naked asm so we control the prologue. */
__attribute__((naked))
void *call_with_user_stack_1(user_stack_fn fn, void *arg)
{
	__asm__ volatile(
		"push {lr}\n"
		/* r3 <- user_stack base
		 * lr <- user_stack base + size - 4 (top sentinel) */
		"ldr r3, =user_stack\n"
		"ldr lr, =" STRINGIFY(USER_STACK_SIZE) "\n"
		"add lr, r3, lr\n"
		"sub lr, lr, #4\n"

		/* If we're already on user_stack, just call fn(arg) without
		 * switching (re-entrancy). r3 = start, lr = end (top). */
		"cmp sp, r3\n"
		"blo not_on_user_stack_us1\n"
		"cmp sp, lr\n"
		"bls already_us1\n"

		"not_on_user_stack_us1:\n"
		/* Save firmware SP in user_stack_exit_sp. */
		"ldr r3, =user_stack_exit_sp\n"
		"str sp, [r3]\n"

		/* Swap to user_stack top (lr currently = user_stack_top - 4). */
		"mov r3, lr\n"
		"mov lr, sp\n"
		"mov sp, r3\n"

		/* Save firmware sp on user_stack, then call fn. r0=fn, r1=arg. */
		"push {lr}\n"
		/* Shift: r3 <- fn, r0 <- arg, then BLX r3 */
		"mov r3, r0\n"
		"mov r0, r1\n"
		"blx r3\n"

		/* Validate canary, return r0 to caller. */
		"push {r0}\n"
		"bl validate_user_stack\n"
		"pop {r0}\n"

		"pop {lr}\n"  /* restore firmware sp into lr */
		"mov sp, lr\n"
		"pop {pc}\n"

		"already_us1:\n"
		/* Already on user_stack; just call fn(arg). */
		"mov r3, r0\n"
		"mov r0, r1\n"
		"blx r3\n"
		"pop {pc}\n"
	);
}

#endif  /* TARGET_PLAYDATE */
