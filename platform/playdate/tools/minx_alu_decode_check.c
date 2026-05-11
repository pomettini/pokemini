/*
 * Host-side equivalence check for a compact MinxCPU XX ALU decoder.
 *
 * This intentionally tests only opcodes 00-3F, the dense A-register ALU
 * block. It compares a reference implementation shaped like the current
 * switch cases with a compact decoder that must preserve Fetch8 side effects.
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
static uint32_t rng_state = 0x703a5eedu;

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
	MinxCPU.HL.D &= MEM_SIZE - 1;
	MinxCPU.X.D &= MEM_SIZE - 1;
	MinxCPU.Y.D &= MEM_SIZE - 1;
	MinxCPU.N.D &= MEM_SIZE - 1;
}

static uint8_t local_read(uint32_t addr)
{
	return MinxCPU_OnRead(1, addr);
}

static int exec_reference_alu(void)
{
	uint8_t I8A;
	uint16_t I16;

	MinxCPU.IR = Fetch8();
	switch (MinxCPU.IR) {
	case 0x00: MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU.BA.B.L); return 8;
	case 0x01: MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU.BA.B.H); return 8;
	case 0x02: I8A = Fetch8(); MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, I8A); return 8;
	case 0x03: MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 8;
	case 0x04: I8A = Fetch8(); MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 12;
	case 0x05: I16 = Fetch16(); MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16)); return 16;
	case 0x06: MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D)); return 8;
	case 0x07: MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 8;

	case 0x08: MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU.BA.B.L); return 8;
	case 0x09: MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU.BA.B.H); return 8;
	case 0x0A: I8A = Fetch8(); MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, I8A); return 8;
	case 0x0B: MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 8;
	case 0x0C: I8A = Fetch8(); MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 12;
	case 0x0D: I16 = Fetch16(); MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16)); return 16;
	case 0x0E: MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D)); return 8;
	case 0x0F: MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 8;

	case 0x10: MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU.BA.B.L); return 8;
	case 0x11: MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU.BA.B.H); return 8;
	case 0x12: I8A = Fetch8(); MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, I8A); return 8;
	case 0x13: MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 8;
	case 0x14: I8A = Fetch8(); MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 12;
	case 0x15: I16 = Fetch16(); MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16)); return 16;
	case 0x16: MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D)); return 8;
	case 0x17: MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 8;

	case 0x18: MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU.BA.B.L); return 8;
	case 0x19: MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU.BA.B.H); return 8;
	case 0x1A: I8A = Fetch8(); MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, I8A); return 8;
	case 0x1B: MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 8;
	case 0x1C: I8A = Fetch8(); MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 12;
	case 0x1D: I16 = Fetch16(); MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16)); return 16;
	case 0x1E: MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D)); return 8;
	case 0x1F: MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 8;

	case 0x20: MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU.BA.B.L); return 8;
	case 0x21: MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU.BA.B.H); return 8;
	case 0x22: I8A = Fetch8(); MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, I8A); return 8;
	case 0x23: MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 8;
	case 0x24: I8A = Fetch8(); MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 12;
	case 0x25: I16 = Fetch16(); MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16)); return 16;
	case 0x26: MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D)); return 8;
	case 0x27: MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 8;

	case 0x28: MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU.BA.B.L); return 8;
	case 0x29: MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU.BA.B.H); return 8;
	case 0x2A: I8A = Fetch8(); MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, I8A); return 8;
	case 0x2B: MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 8;
	case 0x2C: I8A = Fetch8(); MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 12;
	case 0x2D: I16 = Fetch16(); MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16)); return 16;
	case 0x2E: MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D)); return 8;
	case 0x2F: MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 8;

	case 0x30: SUB8(MinxCPU.BA.B.L, MinxCPU.BA.B.L); return 8;
	case 0x31: SUB8(MinxCPU.BA.B.L, MinxCPU.BA.B.H); return 8;
	case 0x32: I8A = Fetch8(); SUB8(MinxCPU.BA.B.L, I8A); return 8;
	case 0x33: SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 8;
	case 0x34: I8A = Fetch8(); SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 12;
	case 0x35: I16 = Fetch16(); SUB8(MinxCPU.BA.B.L, local_read((MinxCPU.HL.B.I << 16) | I16)); return 16;
	case 0x36: SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D)); return 8;
	case 0x37: SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 8;

	case 0x38: MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU.BA.B.L); return 8;
	case 0x39: MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU.BA.B.H); return 8;
	case 0x3A: I8A = Fetch8(); MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, I8A); return 8;
	case 0x3B: MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 8;
	case 0x3C: I8A = Fetch8(); MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 12;
	case 0x3D: I16 = Fetch16(); MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16)); return 16;
	case 0x3E: MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D)); return 8;
	case 0x3F: MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 8;
	default: return -1;
	}
}

static int exec_decoded_alu(void)
{
	uint8_t opcode;
	uint8_t src;
	uint8_t off;
	uint16_t addr16;

	MinxCPU.IR = Fetch8();
	opcode = MinxCPU.IR;
	if (opcode >= 0x40)
		return -1;

	switch (opcode & 0x07) {
	case 0x00:
		src = MinxCPU.BA.B.L;
		break;
	case 0x01:
		src = MinxCPU.BA.B.H;
		break;
	case 0x02:
		src = Fetch8();
		break;
	case 0x03:
		src = MinxCPU_OnRead(1, MinxCPU.HL.D);
		break;
	case 0x04:
		off = Fetch8();
		src = MinxCPU_OnRead(1, MinxCPU.N.D + off);
		break;
	case 0x05:
		addr16 = Fetch16();
		src = (opcode == 0x35)
		    ? local_read((MinxCPU.HL.B.I << 16) | addr16)
		    : MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | addr16);
		break;
	case 0x06:
		src = MinxCPU_OnRead(1, MinxCPU.X.D);
		break;
	default:
		src = MinxCPU_OnRead(1, MinxCPU.Y.D);
		break;
	}

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
	default:
		MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, src);
		break;
	}

	switch (opcode & 0x07) {
	case 0x04:
		return 12;
	case 0x05:
		return 16;
	default:
		return 8;
	}
}

static void dump_cpu(const char *label, const TMinxCPU *cpu)
{
	printf("%s A=%02x B=%02x HL=%08x X=%08x Y=%08x N=%08x PC=%08x F=%02x IR=%02x\n",
	    label,
	    cpu->BA.B.L, cpu->BA.B.H, cpu->HL.D, cpu->X.D, cpu->Y.D,
	    cpu->N.D, cpu->PC.D, cpu->F, cpu->IR);
}

int main(void)
{
	randomize_memory();

	for (uint32_t opcode = 0; opcode < 0x40; opcode++) {
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
			ref_cycles = exec_reference_alu();
			ref_cpu = MinxCPU;

			MinxCPU = start;
			dec_cycles = exec_decoded_alu();
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

	printf("minx_alu_decode_check: passed %u randomized cases\n",
	    0x40u * TRIALS_PER_OPCODE);
	return 0;
}
