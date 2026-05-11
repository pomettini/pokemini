/*
 * Host-side equivalence check for compact MinxCPU CE absolute MOV ops.
 *
 * Tests CE D0-D7 against the current switch semantics.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../source/MinxCPU.h"

#define MEM_SIZE (1u << 20)
#define TRIALS_PER_OPCODE 20000
#define MAX_WRITES 4

typedef struct {
	uint32_t addr;
	uint8_t old_value;
	uint8_t new_value;
} WriteLog;

TMinxCPU MinxCPU;

static uint8_t memory[MEM_SIZE];
static WriteLog writes[MAX_WRITES];
static int write_count;
static uint32_t rng_state = 0xced0d7u;

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

static uint8_t local_read(uint32_t addr)
{
	return MinxCPU_OnRead(1, addr);
}

static int exec_reference_ce_abs_mov(void)
{
	uint16_t I16;

	MinxCPU.IR = Fetch8();
	switch (MinxCPU.IR) {
	case 0xD0:
		I16 = Fetch16();
		MinxCPU.BA.B.L = local_read(I16);
		return 20;
	case 0xD1:
		I16 = Fetch16();
		MinxCPU.BA.B.H = MinxCPU_OnRead(1, I16);
		return 20;
	case 0xD2:
		I16 = Fetch16();
		MinxCPU.HL.B.L = MinxCPU_OnRead(1, I16);
		return 20;
	case 0xD3:
		I16 = Fetch16();
		MinxCPU.HL.B.H = MinxCPU_OnRead(1, I16);
		return 20;
	case 0xD4:
		I16 = Fetch16();
		MinxCPU_OnWrite(1, I16, MinxCPU.BA.B.L);
		return 20;
	case 0xD5:
		I16 = Fetch16();
		MinxCPU_OnWrite(1, I16, MinxCPU.BA.B.H);
		return 20;
	case 0xD6:
		I16 = Fetch16();
		MinxCPU_OnWrite(1, I16, MinxCPU.HL.B.L);
		return 20;
	case 0xD7:
		I16 = Fetch16();
		MinxCPU_OnWrite(1, I16, MinxCPU.HL.B.H);
		return 20;
	default:
		return -1;
	}
}

static int exec_decoded_ce_abs_mov(void)
{
	uint8_t opcode;
	uint16_t addr;
	uint8_t value;

	MinxCPU.IR = Fetch8();
	opcode = MinxCPU.IR;
	if ((opcode & 0xF8) != 0xD0)
		return -1;

	addr = Fetch16();
	if ((opcode & 0x04) == 0) {
		value = (opcode == 0xD0) ? local_read(addr) : MinxCPU_OnRead(1, addr);
		switch (opcode & 0x03) {
		case 0x00: MinxCPU.BA.B.L = value; break;
		case 0x01: MinxCPU.BA.B.H = value; break;
		case 0x02: MinxCPU.HL.B.L = value; break;
		default: MinxCPU.HL.B.H = value; break;
		}
	} else {
		switch (opcode & 0x03) {
		case 0x00: value = MinxCPU.BA.B.L; break;
		case 0x01: value = MinxCPU.BA.B.H; break;
		case 0x02: value = MinxCPU.HL.B.L; break;
		default: value = MinxCPU.HL.B.H; break;
		}
		MinxCPU_OnWrite(1, addr, value);
	}
	return 20;
}

static void dump_cpu(const char *label, const TMinxCPU *cpu)
{
	printf("%s A=%02x B=%02x HL=%08x PC=%08x F=%02x IR=%02x\n",
	    label, cpu->BA.B.L, cpu->BA.B.H, cpu->HL.D, cpu->PC.D, cpu->F, cpu->IR);
}

int main(void)
{
	randomize_memory();

	for (uint32_t opcode = 0xD0; opcode <= 0xD7; opcode++) {
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
			ref_cycles = exec_reference_ce_abs_mov();
			ref_cpu = MinxCPU;
			ref_write_count = write_count;
			memcpy(ref_writes, writes, sizeof(ref_writes));
			restore_writes(ref_writes, ref_write_count);

			MinxCPU = start;
			reset_write_log();
			dec_cycles = exec_decoded_ce_abs_mov();
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

	printf("minx_ce_abs_mov_check: passed %u randomized cases\n",
	    8u * TRIALS_PER_OPCODE);
	return 0;
}
