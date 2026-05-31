/*
  Runtime DTCM allocator. See PokeMini_DTCM.h.
*/

#include "PokeMini_DTCM.h"

#include <stdint.h>
#include <stddef.h>

#if defined(TARGET_PLAYDATE) && POKEMINI_PM_RAM_DTCM

static uint8_t *dtcm_mempool = NULL;
static uint8_t *dtcm_mempool_start = NULL;

void dtcm_init(void *frame_ptr_at_startup)
{
	// The frame pointer at startup is a high address inside DTCM (since the
	// firmware-provided stack is in DTCM, growing down). Subtracting the
	// reserve gives the highest address the app's stack should never grow
	// past. Below that is reserved-but-unused DTCM.
	dtcm_mempool_start = (uint8_t *)frame_ptr_at_startup
	                   - POKEMINI_DTCM_STACK_RESERVE;
	dtcm_mempool = dtcm_mempool_start;
}

void *dtcm_alloc(size_t size)
{
	if (!dtcm_mempool) return NULL;
	void *p = dtcm_mempool;
	dtcm_mempool += size;
	return p;
}

void *dtcm_alloc_aligned(size_t size, size_t alignment)
{
	if (!dtcm_mempool) return NULL;
	uintptr_t addr = (uintptr_t)dtcm_mempool;
	uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
	dtcm_mempool = (uint8_t *)(aligned + size);
	return (void *)aligned;
}

void *dtcm_get_mempool(void)
{
	return dtcm_mempool;
}

#else

// Stubs for non-DTCM builds so source files including this don't break.
void dtcm_init(void *frame_ptr_at_startup) { (void)frame_ptr_at_startup; }
void *dtcm_alloc(size_t size) { (void)size; return NULL; }
void *dtcm_alloc_aligned(size_t size, size_t alignment)
{
	(void)size; (void)alignment; return NULL;
}
void *dtcm_get_mempool(void) { return NULL; }

#endif
