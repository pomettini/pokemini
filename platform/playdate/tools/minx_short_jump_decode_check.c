/*
 * Host-side equivalence check for compact MinxCPU XX short conditional jumps.
 *
 * Tests opcodes E4-E7 against the current switch semantics.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../source/MinxCPU.h"

#define MEM_SIZE (1u << 20)
#define TRIALS_PER_OPCODE 20000

TMinxCPU MinxCPU;

static uint8_t memory[MEM_SIZE];
static uint32_t rng_state = 0xe4e7feedu;

uint8_t MinxCPU_OnRead(int cpu, uint32_t addr)
{
	(void)cpu;
	return memory[addr & (MEM_SIZE - 1)];
}

void MinxCPU_OnWrite(int cpu, uint32_t addr, uint8_t data)
{
	(void)cpu;
	memory[addr & (MEM_SIZE - 1)] = data;
}

void MinxCPU_OnException(int type, uint32_t opc)
{
	fprintf(stderr, "unexpected exception type=%d opc=%08x\n", type, opc);
	abort();
}

void MinxCPU_OnSleep(int type)
{
	(void)type;
}

void MinxCPU_OnIRQHandle(uint8_t flag, uint8_t shift_u)
{
	(void)flag;
	(void)shift_u;
}

void MinxCPU_OnIRQAct(uint8_t intr)
{
	(void)intr;
}

static uint32_t rnd32(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

static uint8_t rnd8(void)
{
	return (uint8_t)rnd32();
}

static void randomize_memory(void)
{
	for (uint32_t i = 0; i < MEM_SIZE; i++)
		memory[i] = rnd8();
}

static void randomize_cpu(void)
{
	uint8_t *raw = (uint8_t *)&MinxCPU;
	for (size_t i = 0; i < sizeof(MinxCPU); i++)
		raw[i] = rnd8();

	MinxCPU.Status = MINX_STATUS_NORMAL;
	MinxCPU.Shift_U = 0;
	MinxCPU.PC.W.L = (uint16_t)(rnd32() & 0x7ffc);
	MinxCPU.PC.B.I = rnd8();
}

static int exec_reference_short_jump(void)
{
	uint8_t I8A;

	MinxCPU.IR = Fetch8();
	switch (MinxCPU.IR) {
	case 0xE4:
		I8A = Fetch8();
		if (MinxCPU.F & MINX_FLAG_CARRY)
			JMPS(S8_TO_16(I8A));
		return 8;
	case 0xE5:
		I8A = Fetch8();
		if (!(MinxCPU.F & MINX_FLAG_CARRY))
			JMPS(S8_TO_16(I8A));
		return 8;
	case 0xE6:
		I8A = Fetch8();
		if (MinxCPU.F & MINX_FLAG_ZERO)
			JMPS(S8_TO_16(I8A));
		return 8;
	case 0xE7:
		I8A = Fetch8();
		if (!(MinxCPU.F & MINX_FLAG_ZERO))
			JMPS(S8_TO_16(I8A));
		return 8;
	default:
		return -1;
	}
}

static int exec_decoded_short_jump(void)
{
	uint8_t opcode;
	uint8_t offset;
	int taken;

	MinxCPU.IR = Fetch8();
	opcode = MinxCPU.IR;
	if ((opcode & 0xFC) != 0xE4)
		return -1;

	offset = Fetch8();
	switch (opcode & 0x03) {
	case 0x00:
		taken = (MinxCPU.F & MINX_FLAG_CARRY) != 0;
		break;
	case 0x01:
		taken = (MinxCPU.F & MINX_FLAG_CARRY) == 0;
		break;
	case 0x02:
		taken = (MinxCPU.F & MINX_FLAG_ZERO) != 0;
		break;
	default:
		taken = (MinxCPU.F & MINX_FLAG_ZERO) == 0;
		break;
	}
	if (taken) {
		MinxCPU.PC.B.I = MinxCPU.U1;
		MinxCPU.U2 = MinxCPU.U1;
		MinxCPU.PC.W.L = (uint16_t)(MinxCPU.PC.W.L + S8_TO_16(offset) - 1);
	}
	return 8;
}

static void dump_cpu(const char *label, const TMinxCPU *cpu)
{
	printf("%s PC=%08x U1=%02x U2=%02x F=%02x IR=%02x\n",
	    label, cpu->PC.D, cpu->U1, cpu->U2, cpu->F, cpu->IR);
}

int main(void)
{
	randomize_memory();

	for (uint32_t opcode = 0xE4; opcode <= 0xE7; opcode++) {
		for (uint32_t trial = 0; trial < TRIALS_PER_OPCODE; trial++) {
			TMinxCPU start;
			TMinxCPU ref_cpu;
			TMinxCPU dec_cpu;
			uint8_t saved_bytes[4];
			uint16_t pc;
			int ref_cycles;
			int dec_cycles;

			randomize_cpu();
			pc = MinxCPU.PC.W.L;
			for (int i = 0; i < 4; i++)
				saved_bytes[i] = memory[(pc + i) & (MEM_SIZE - 1)];
			memory[pc & (MEM_SIZE - 1)] = (uint8_t)opcode;

			start = MinxCPU;

			MinxCPU = start;
			ref_cycles = exec_reference_short_jump();
			ref_cpu = MinxCPU;

			MinxCPU = start;
			dec_cycles = exec_decoded_short_jump();
			dec_cpu = MinxCPU;

			for (int i = 0; i < 4; i++)
				memory[(pc + i) & (MEM_SIZE - 1)] = saved_bytes[i];

			if (ref_cycles != dec_cycles || memcmp(&ref_cpu, &dec_cpu, sizeof(ref_cpu)) != 0) {
				printf("mismatch opcode=%02x trial=%u ref_cycles=%d dec_cycles=%d\n",
				    opcode, trial, ref_cycles, dec_cycles);
				dump_cpu("start", &start);
				dump_cpu("ref  ", &ref_cpu);
				dump_cpu("dec  ", &dec_cpu);
				return 1;
			}
		}
	}

	printf("minx_short_jump_decode_check: passed %u randomized cases\n",
	    4u * TRIALS_PER_OPCODE);
	return 0;
}
