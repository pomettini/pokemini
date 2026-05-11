/*
 * Host-side equivalence check for compact MinxCPU CE [X+#ss] -> A ops.
 *
 * Tests CE 00/08/10/18/20/28/30/38/40 against the current switch semantics.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../source/MinxCPU.h"

#define MEM_SIZE (1u << 20)
#define TRIALS_PER_OPCODE 20000

TMinxCPU MinxCPU;

static const uint8_t test_opcodes[] = {
	0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40
};

static uint8_t memory[MEM_SIZE];
static uint32_t rng_state = 0xce0040u;

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
	MinxCPU.X.D &= MEM_SIZE - 1;
}

static int exec_reference_ce_xshort_a(void)
{
	uint8_t I8A;
	uint16_t I16;

	MinxCPU.IR = Fetch8();
	switch (MinxCPU.IR) {
	case 0x00:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16));
		return 16;
	case 0x08:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16));
		return 16;
	case 0x10:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16));
		return 16;
	case 0x18:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16));
		return 16;
	case 0x20:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16));
		return 16;
	case 0x28:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16));
		return 16;
	case 0x30:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16));
		return 16;
	case 0x38:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16));
		return 16;
	case 0x40:
		I8A = Fetch8();
		I16 = MinxCPU.X.W.L + S8_TO_16(I8A);
		MinxCPU.BA.B.L = MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | I16);
		return 16;
	default:
		return -1;
	}
}

static int exec_decoded_ce_xshort_a(void)
{
	uint8_t opcode;
	uint8_t src;
	uint8_t offset;
	uint16_t addr;

	MinxCPU.IR = Fetch8();
	opcode = MinxCPU.IR;
	if ((opcode & 0x07) != 0 || opcode > 0x40)
		return -1;

	offset = Fetch8();
	addr = MinxCPU.X.W.L + S8_TO_16(offset);
	src = MinxCPU_OnRead(1, (MinxCPU.X.B.I << 16) | addr);

	switch (opcode >> 3) {
	case 0:
		MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, src);
		break;
	case 1:
		MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, src);
		break;
	case 2:
		MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, src);
		break;
	case 3:
		MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, src);
		break;
	case 4:
		MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, src);
		break;
	case 5:
		MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, src);
		break;
	case 6:
		(void)SUB8(MinxCPU.BA.B.L, src);
		break;
	case 7:
		MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, src);
		break;
	default:
		MinxCPU.BA.B.L = src;
		break;
	}
	return 16;
}

static void dump_cpu(const char *label, const TMinxCPU *cpu)
{
	printf("%s A=%02x B=%02x X=%08x PC=%08x F=%02x IR=%02x\n",
	    label, cpu->BA.B.L, cpu->BA.B.H, cpu->X.D, cpu->PC.D, cpu->F, cpu->IR);
}

int main(void)
{
	randomize_memory();

	for (size_t opcode_idx = 0; opcode_idx < sizeof(test_opcodes); opcode_idx++) {
		uint8_t opcode = test_opcodes[opcode_idx];
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
			memory[pc & (MEM_SIZE - 1)] = opcode;

			start = MinxCPU;

			MinxCPU = start;
			ref_cycles = exec_reference_ce_xshort_a();
			ref_cpu = MinxCPU;

			MinxCPU = start;
			dec_cycles = exec_decoded_ce_xshort_a();
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

	printf("minx_ce_xshort_a_check: passed %u randomized cases\n",
	    (unsigned)(sizeof(test_opcodes) * TRIALS_PER_OPCODE));
	return 0;
}
