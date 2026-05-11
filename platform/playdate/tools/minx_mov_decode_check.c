/*
 * Host-side equivalence check for a compact MinxCPU XX MOV decoder.
 *
 * Tests opcodes 40-7F against the current switch semantics, including memory
 * writes and the odd 7C "NOTHING #nn" case.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../source/MinxCPU.h"

#define MEM_SIZE (1u << 20)
#define TRIALS_PER_OPCODE 20000
#define MAX_WRITES 8

typedef struct {
	uint32_t addr;
	uint8_t old_value;
	uint8_t new_value;
} WriteLog;

TMinxCPU MinxCPU;

static uint8_t memory[MEM_SIZE];
static WriteLog writes[MAX_WRITES];
static int write_count;
static uint32_t rng_state = 0x40607eedu;

uint8_t MinxCPU_OnRead(int cpu, uint32_t addr)
{
	(void)cpu;
	return memory[addr & (MEM_SIZE - 1)];
}

void MinxCPU_OnWrite(int cpu, uint32_t addr, uint8_t data)
{
	uint32_t masked = addr & (MEM_SIZE - 1);

	(void)cpu;
	if (write_count >= MAX_WRITES) {
		fprintf(stderr, "too many writes\n");
		abort();
	}
	writes[write_count].addr = masked;
	writes[write_count].old_value = memory[masked];
	writes[write_count].new_value = data;
	write_count++;
	memory[masked] = data;
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

static void reset_write_log(void)
{
	write_count = 0;
}

static void restore_writes(const WriteLog *log, int count)
{
	for (int i = count - 1; i >= 0; i--)
		memory[log[i].addr] = log[i].old_value;
}

static int writes_match(const WriteLog *a, int a_count, const WriteLog *b, int b_count)
{
	if (a_count != b_count)
		return 0;
	for (int i = 0; i < a_count; i++) {
		if (a[i].addr != b[i].addr || a[i].new_value != b[i].new_value)
			return 0;
	}
	return 1;
}

static uint8_t mov_src(uint8_t src)
{
	uint8_t off;

	switch (src) {
	case 0: return MinxCPU.BA.B.L;
	case 1: return MinxCPU.BA.B.H;
	case 2: return MinxCPU.HL.B.L;
	case 3: return MinxCPU.HL.B.H;
	case 4:
		off = Fetch8();
		return MinxCPU_OnRead(1, MinxCPU.N.D + off);
	case 5: return MinxCPU_OnRead(1, MinxCPU.HL.D);
	case 6: return MinxCPU_OnRead(1, MinxCPU.X.D);
	default: return MinxCPU_OnRead(1, MinxCPU.Y.D);
	}
}

static int exec_reference_mov(void)
{
	uint8_t I8A;

	MinxCPU.IR = Fetch8();
	switch (MinxCPU.IR) {
	case 0x40: return 4;
	case 0x41: MinxCPU.BA.B.L = MinxCPU.BA.B.H; return 4;
	case 0x42: MinxCPU.BA.B.L = MinxCPU.HL.B.L; return 4;
	case 0x43: MinxCPU.BA.B.L = MinxCPU.HL.B.H; return 4;
	case 0x44: I8A = Fetch8(); MinxCPU.BA.B.L = MinxCPU_OnRead(1, MinxCPU.N.D + I8A); return 12;
	case 0x45: MinxCPU.BA.B.L = MinxCPU_OnRead(1, MinxCPU.HL.D); return 8;
	case 0x46: MinxCPU.BA.B.L = MinxCPU_OnRead(1, MinxCPU.X.D); return 8;
	case 0x47: MinxCPU.BA.B.L = MinxCPU_OnRead(1, MinxCPU.Y.D); return 8;

	case 0x48: MinxCPU.BA.B.H = MinxCPU.BA.B.L; return 4;
	case 0x49: return 4;
	case 0x4A: MinxCPU.BA.B.H = MinxCPU.HL.B.L; return 4;
	case 0x4B: MinxCPU.BA.B.H = MinxCPU.HL.B.H; return 4;
	case 0x4C: I8A = Fetch8(); MinxCPU.BA.B.H = MinxCPU_OnRead(1, MinxCPU.N.D + I8A); return 12;
	case 0x4D: MinxCPU.BA.B.H = MinxCPU_OnRead(1, MinxCPU.HL.D); return 8;
	case 0x4E: MinxCPU.BA.B.H = MinxCPU_OnRead(1, MinxCPU.X.D); return 8;
	case 0x4F: MinxCPU.BA.B.H = MinxCPU_OnRead(1, MinxCPU.Y.D); return 8;

	case 0x50: MinxCPU.HL.B.L = MinxCPU.BA.B.L; return 4;
	case 0x51: MinxCPU.HL.B.L = MinxCPU.BA.B.H; return 4;
	case 0x52: return 4;
	case 0x53: MinxCPU.HL.B.L = MinxCPU.HL.B.H; return 4;
	case 0x54: I8A = Fetch8(); MinxCPU.HL.B.L = MinxCPU_OnRead(1, MinxCPU.N.D + I8A); return 12;
	case 0x55: MinxCPU.HL.B.L = MinxCPU_OnRead(1, MinxCPU.HL.D); return 8;
	case 0x56: MinxCPU.HL.B.L = MinxCPU_OnRead(1, MinxCPU.X.D); return 8;
	case 0x57: MinxCPU.HL.B.L = MinxCPU_OnRead(1, MinxCPU.Y.D); return 8;

	case 0x58: MinxCPU.HL.B.H = MinxCPU.BA.B.L; return 4;
	case 0x59: MinxCPU.HL.B.H = MinxCPU.BA.B.H; return 4;
	case 0x5A: MinxCPU.HL.B.H = MinxCPU.HL.B.L; return 4;
	case 0x5B: return 4;
	case 0x5C: I8A = Fetch8(); MinxCPU.HL.B.H = MinxCPU_OnRead(1, MinxCPU.N.D + I8A); return 12;
	case 0x5D: MinxCPU.HL.B.H = MinxCPU_OnRead(1, MinxCPU.HL.D); return 8;
	case 0x5E: MinxCPU.HL.B.H = MinxCPU_OnRead(1, MinxCPU.X.D); return 8;
	case 0x5F: MinxCPU.HL.B.H = MinxCPU_OnRead(1, MinxCPU.Y.D); return 8;

	case 0x60: MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU.BA.B.L); return 8;
	case 0x61: MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU.BA.B.H); return 8;
	case 0x62: MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU.HL.B.L); return 8;
	case 0x63: MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU.HL.B.H); return 8;
	case 0x64: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 16;
	case 0x65: MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 12;
	case 0x66: MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU_OnRead(1, MinxCPU.X.D)); return 12;
	case 0x67: MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 12;

	case 0x68: MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.BA.B.L); return 8;
	case 0x69: MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.BA.B.H); return 8;
	case 0x6A: MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.HL.B.L); return 8;
	case 0x6B: MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.HL.B.H); return 8;
	case 0x6C: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 16;
	case 0x6D: MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 12;
	case 0x6E: MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU_OnRead(1, MinxCPU.X.D)); return 12;
	case 0x6F: MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 12;

	case 0x70: MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU.BA.B.L); return 8;
	case 0x71: MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU.BA.B.H); return 8;
	case 0x72: MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU.HL.B.L); return 8;
	case 0x73: MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU.HL.B.H); return 8;
	case 0x74: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU_OnRead(1, MinxCPU.N.D + I8A)); return 16;
	case 0x75: MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 12;
	case 0x76: MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU_OnRead(1, MinxCPU.X.D)); return 12;
	case 0x77: MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 12;

	case 0x78: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU.BA.B.L); return 8;
	case 0x79: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU.BA.B.H); return 8;
	case 0x7A: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU.HL.B.L); return 8;
	case 0x7B: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU.HL.B.H); return 8;
	case 0x7C: I8A = Fetch8(); (void)I8A; return 64;
	case 0x7D: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 16;
	case 0x7E: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU_OnRead(1, MinxCPU.X.D)); return 16;
	case 0x7F: I8A = Fetch8(); MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 16;
	default: return -1;
	}
}

static int exec_decoded_mov(void)
{
	uint8_t opcode;
	uint8_t src;
	uint8_t dst;
	uint8_t off;

	MinxCPU.IR = Fetch8();
	opcode = MinxCPU.IR;
	if (opcode < 0x40 || opcode >= 0x80)
		return -1;

	dst = (opcode >> 3) & 0x07;
	if (dst == 7) {
		off = Fetch8();
		switch (opcode & 0x07) {
		case 0: MinxCPU_OnWrite(1, MinxCPU.N.D + off, MinxCPU.BA.B.L); return 8;
		case 1: MinxCPU_OnWrite(1, MinxCPU.N.D + off, MinxCPU.BA.B.H); return 8;
		case 2: MinxCPU_OnWrite(1, MinxCPU.N.D + off, MinxCPU.HL.B.L); return 8;
		case 3: MinxCPU_OnWrite(1, MinxCPU.N.D + off, MinxCPU.HL.B.H); return 8;
		case 4: return 64;
		case 5: MinxCPU_OnWrite(1, MinxCPU.N.D + off, MinxCPU_OnRead(1, MinxCPU.HL.D)); return 16;
		case 6: MinxCPU_OnWrite(1, MinxCPU.N.D + off, MinxCPU_OnRead(1, MinxCPU.X.D)); return 16;
		default: MinxCPU_OnWrite(1, MinxCPU.N.D + off, MinxCPU_OnRead(1, MinxCPU.Y.D)); return 16;
		}
	}

	src = mov_src(opcode & 0x07);
	switch (dst) {
	case 0:
		MinxCPU.BA.B.L = src;
		break;
	case 1:
		MinxCPU.BA.B.H = src;
		break;
	case 2:
		MinxCPU.HL.B.L = src;
		break;
	case 3:
		MinxCPU.HL.B.H = src;
		break;
	case 4:
		MinxCPU_OnWrite(1, MinxCPU.X.D, src);
		break;
	case 5:
		MinxCPU_OnWrite(1, MinxCPU.HL.D, src);
		break;
	default:
		MinxCPU_OnWrite(1, MinxCPU.Y.D, src);
		break;
	}

	if (dst < 4)
		return ((opcode & 0x07) < 4) ? 4 : ((opcode & 0x07) == 4 ? 12 : 8);
	return ((opcode & 0x07) < 4) ? 8 : ((opcode & 0x07) == 4 ? 16 : 12);
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

	for (uint32_t opcode = 0x40; opcode < 0x80; opcode++) {
		for (uint32_t trial = 0; trial < TRIALS_PER_OPCODE; trial++) {
			TMinxCPU start;
			TMinxCPU ref_cpu;
			TMinxCPU dec_cpu;
			WriteLog ref_writes[MAX_WRITES];
			WriteLog dec_writes[MAX_WRITES];
			uint8_t saved_bytes[4];
			uint16_t pc;
			int ref_write_count;
			int dec_write_count;
			int ref_cycles;
			int dec_cycles;

			randomize_cpu();
			pc = MinxCPU.PC.W.L;
			for (int i = 0; i < 4; i++)
				saved_bytes[i] = memory[(pc + i) & (MEM_SIZE - 1)];
			memory[pc & (MEM_SIZE - 1)] = (uint8_t)opcode;

			start = MinxCPU;

			MinxCPU = start;
			reset_write_log();
			ref_cycles = exec_reference_mov();
			ref_cpu = MinxCPU;
			ref_write_count = write_count;
			memcpy(ref_writes, writes, sizeof(ref_writes));
			restore_writes(ref_writes, ref_write_count);

			MinxCPU = start;
			reset_write_log();
			dec_cycles = exec_decoded_mov();
			dec_cpu = MinxCPU;
			dec_write_count = write_count;
			memcpy(dec_writes, writes, sizeof(dec_writes));
			restore_writes(dec_writes, dec_write_count);

			for (int i = 0; i < 4; i++)
				memory[(pc + i) & (MEM_SIZE - 1)] = saved_bytes[i];

			if (ref_cycles != dec_cycles ||
			    memcmp(&ref_cpu, &dec_cpu, sizeof(ref_cpu)) != 0 ||
			    !writes_match(ref_writes, ref_write_count, dec_writes, dec_write_count)) {
				printf("mismatch opcode=%02x trial=%u ref_cycles=%d dec_cycles=%d ref_writes=%d dec_writes=%d\n",
				    opcode, trial, ref_cycles, dec_cycles, ref_write_count, dec_write_count);
				dump_cpu("start", &start);
				dump_cpu("ref  ", &ref_cpu);
				dump_cpu("dec  ", &dec_cpu);
				return 1;
			}
		}
	}

	printf("minx_mov_decode_check: passed %u randomized cases\n",
	    0x40u * TRIALS_PER_OPCODE);
	return 0;
}
