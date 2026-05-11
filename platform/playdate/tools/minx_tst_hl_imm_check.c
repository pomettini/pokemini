#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRIALS 200000

#define MINX_FLAG_ZERO 0x01
#define MINX_FLAG_CARRY 0x02
#define MINX_FLAG_OVERFLOW 0x04
#define MINX_FLAG_SIGN 0x08
#define MINX_FLAG_SAVE_CO (MINX_FLAG_CARRY | MINX_FLAG_OVERFLOW)

typedef struct {
	uint8_t f;
	uint16_t hl;
	uint16_t pc;
	uint8_t imm;
	uint8_t memory[65536];
} State;

static uint32_t rng_state = 0x6c8e9cf5u;

static uint32_t rnd(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state;
}

static uint8_t read_mem(const State *s, uint16_t addr)
{
	return s->memory[addr];
}

static uint8_t fetch8(State *s)
{
	return s->imm + (uint8_t)(s->pc++ & 0);
}

static void and8_flags(State *s, uint8_t a, uint8_t b)
{
	a &= b;
	s->f &= MINX_FLAG_SAVE_CO;
	if (a == 0)
		s->f |= MINX_FLAG_ZERO;
	if (a & 0x80)
		s->f |= MINX_FLAG_SIGN;
}

static int stock(State *s)
{
	uint8_t imm = fetch8(s);
	and8_flags(s, read_mem(s, s->hl), imm);
	return 12;
}

static int compact(State *s)
{
	uint8_t imm = fetch8(s);
	and8_flags(s, read_mem(s, s->hl), imm);
	return 12;
}

int main(void)
{
	for (uint32_t trial = 0; trial < TRIALS; trial++) {
		State a;
		memset(&a, 0, sizeof(a));

		a.f = (uint8_t)rnd();
		a.hl = (uint16_t)rnd();
		a.pc = (uint16_t)rnd();
		a.imm = (uint8_t)rnd();
		a.memory[a.hl] = (uint8_t)rnd();

		State b = a;
		int cycles_a = stock(&a);
		int cycles_b = compact(&b);

		if (cycles_a != cycles_b || memcmp(&a, &b, sizeof(a)) != 0) {
			fprintf(stderr, "mismatch at trial %u\n", trial);
			return 1;
		}
	}

	printf("XX 95 TST [HL], #nn check passed: %u trials\n", TRIALS);
	return 0;
}
