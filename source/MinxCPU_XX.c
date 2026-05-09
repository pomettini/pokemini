/*
  PokeMini - Pok�mon-Mini Emulator
  Copyright (C) 2009-2012  JustBurn

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Note: Any write to MinxCPU.HL.B.I needs to be reflected into MinxCPU.N.B.I

#include "PokeMini.h"
#include "MinxCPU.h"

#if defined(POKEMINI_CPU_FASTMEM) && defined(PERFORMANCE)
#define MinxCPU_OnRead MinxCPU_FastRead
#define MinxCPU_OnWrite MinxCPU_FastWrite
#endif

#ifdef PD_OPCODE_DIAG
uint32_t MinxCPU_OpcodeDiag[MINXCPU_OPDIAG_TABLES][256];

void MinxCPU_OpcodeDiagReset(void)
{
	int table;
	int opcode;

	for (table = 0; table < MINXCPU_OPDIAG_TABLES; table++)
		for (opcode = 0; opcode < 256; opcode++)
			MinxCPU_OpcodeDiag[table][opcode] = 0;
}
#endif

POKEMINI_HOT_EXEC int MinxCPU_Exec(void)
{
	uint8_t I8A, I8B;
	uint16_t I16;

	// Shift U
	if (MinxCPU.Shift_U) {
		MinxCPU.U1 = MinxCPU.U2;
		MinxCPU.U2 = MinxCPU.PC.B.I;
		MinxCPU.Shift_U--;
		MinxCPU_OnIRQHandle(MinxCPU.F, MinxCPU.Shift_U);
	}

	// Check HALT or STOP status
	if (MinxCPU.Status != MINX_STATUS_NORMAL) {
		if (MinxCPU.Status == MINX_STATUS_IRQ) {
			MinxCPU.Status = MINX_STATUS_NORMAL;	// Return to normal
			CALLI(MinxCPU.IRQ_Vector);		// Jump to IRQ vector
			return 20;
		} else {
			return 8;				// Cause short NOPs
		}
	}

	// Read IR
	MinxCPU.IR = Fetch8();
#ifdef PD_OPCODE_DIAG
	MinxCPU_OpcodeDiag[MINXCPU_OPDIAG_XX][MinxCPU.IR]++;
#endif

	// Process instruction
#if defined(POKEMINI_COMPUTED_GOTO) && defined(__GNUC__)
#define OP(n) op_##n:
#define OP_DEFAULT op_default:
	static const void *const handlers[256] = {
		&&op_00,
		&&op_01,
		&&op_02,
		&&op_03,
		&&op_04,
		&&op_05,
		&&op_06,
		&&op_07,
		&&op_08,
		&&op_09,
		&&op_0A,
		&&op_0B,
		&&op_0C,
		&&op_0D,
		&&op_0E,
		&&op_0F,
		&&op_10,
		&&op_11,
		&&op_12,
		&&op_13,
		&&op_14,
		&&op_15,
		&&op_16,
		&&op_17,
		&&op_18,
		&&op_19,
		&&op_1A,
		&&op_1B,
		&&op_1C,
		&&op_1D,
		&&op_1E,
		&&op_1F,
		&&op_20,
		&&op_21,
		&&op_22,
		&&op_23,
		&&op_24,
		&&op_25,
		&&op_26,
		&&op_27,
		&&op_28,
		&&op_29,
		&&op_2A,
		&&op_2B,
		&&op_2C,
		&&op_2D,
		&&op_2E,
		&&op_2F,
		&&op_30,
		&&op_31,
		&&op_32,
		&&op_33,
		&&op_34,
		&&op_35,
		&&op_36,
		&&op_37,
		&&op_38,
		&&op_39,
		&&op_3A,
		&&op_3B,
		&&op_3C,
		&&op_3D,
		&&op_3E,
		&&op_3F,
		&&op_40,
		&&op_41,
		&&op_42,
		&&op_43,
		&&op_44,
		&&op_45,
		&&op_46,
		&&op_47,
		&&op_48,
		&&op_49,
		&&op_4A,
		&&op_4B,
		&&op_4C,
		&&op_4D,
		&&op_4E,
		&&op_4F,
		&&op_50,
		&&op_51,
		&&op_52,
		&&op_53,
		&&op_54,
		&&op_55,
		&&op_56,
		&&op_57,
		&&op_58,
		&&op_59,
		&&op_5A,
		&&op_5B,
		&&op_5C,
		&&op_5D,
		&&op_5E,
		&&op_5F,
		&&op_60,
		&&op_61,
		&&op_62,
		&&op_63,
		&&op_64,
		&&op_65,
		&&op_66,
		&&op_67,
		&&op_68,
		&&op_69,
		&&op_6A,
		&&op_6B,
		&&op_6C,
		&&op_6D,
		&&op_6E,
		&&op_6F,
		&&op_70,
		&&op_71,
		&&op_72,
		&&op_73,
		&&op_74,
		&&op_75,
		&&op_76,
		&&op_77,
		&&op_78,
		&&op_79,
		&&op_7A,
		&&op_7B,
		&&op_7C,
		&&op_7D,
		&&op_7E,
		&&op_7F,
		&&op_80,
		&&op_81,
		&&op_82,
		&&op_83,
		&&op_84,
		&&op_85,
		&&op_86,
		&&op_87,
		&&op_88,
		&&op_89,
		&&op_8A,
		&&op_8B,
		&&op_8C,
		&&op_8D,
		&&op_8E,
		&&op_8F,
		&&op_90,
		&&op_91,
		&&op_92,
		&&op_93,
		&&op_94,
		&&op_95,
		&&op_96,
		&&op_97,
		&&op_98,
		&&op_99,
		&&op_9A,
		&&op_9B,
		&&op_9C,
		&&op_9D,
		&&op_9E,
		&&op_9F,
		&&op_A0,
		&&op_A1,
		&&op_A2,
		&&op_A3,
		&&op_A4,
		&&op_A5,
		&&op_A6,
		&&op_A7,
		&&op_A8,
		&&op_A9,
		&&op_AA,
		&&op_AB,
		&&op_AC,
		&&op_AD,
		&&op_AE,
		&&op_AF,
		&&op_B0,
		&&op_B1,
		&&op_B2,
		&&op_B3,
		&&op_B4,
		&&op_B5,
		&&op_B6,
		&&op_B7,
		&&op_B8,
		&&op_B9,
		&&op_BA,
		&&op_BB,
		&&op_BC,
		&&op_BD,
		&&op_BE,
		&&op_BF,
		&&op_C0,
		&&op_C1,
		&&op_C2,
		&&op_C3,
		&&op_C4,
		&&op_C5,
		&&op_C6,
		&&op_C7,
		&&op_C8,
		&&op_C9,
		&&op_CA,
		&&op_CB,
		&&op_CC,
		&&op_CD,
		&&op_CE,
		&&op_CF,
		&&op_D0,
		&&op_D1,
		&&op_D2,
		&&op_D3,
		&&op_D4,
		&&op_D5,
		&&op_D6,
		&&op_D7,
		&&op_D8,
		&&op_D9,
		&&op_DA,
		&&op_DB,
		&&op_DC,
		&&op_DD,
		&&op_DE,
		&&op_DF,
		&&op_E0,
		&&op_E1,
		&&op_E2,
		&&op_E3,
		&&op_E4,
		&&op_E5,
		&&op_E6,
		&&op_E7,
		&&op_E8,
		&&op_E9,
		&&op_EA,
		&&op_EB,
		&&op_EC,
		&&op_ED,
		&&op_EE,
		&&op_EF,
		&&op_F0,
		&&op_F1,
		&&op_F2,
		&&op_F3,
		&&op_F4,
		&&op_F5,
		&&op_F6,
		&&op_F7,
		&&op_F8,
		&&op_F9,
		&&op_FA,
		&&op_FB,
		&&op_FC,
		&&op_FD,
		&&op_FE,
		&&op_FF
	};
	(void)&&op_default;
	goto *handlers[MinxCPU.IR];
#else
#define OP(n) case 0x##n:
#define OP_DEFAULT default:
	switch(MinxCPU.IR) {
#endif

		OP(00) // ADD A, A
			MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU.BA.B.L);
			return 8;
		OP(01) // ADD A, B
			MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(02) // ADD A, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(03) // ADD A, [HL]
			MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 8;
		OP(04) // ADD A, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 12;
		OP(05) // ADD A, [#nnnn]
			I16 = Fetch16();
			MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16));
			return 16;
		OP(06) // ADD A, [X]
			MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 8;
		OP(07) // ADD A, [Y]
			MinxCPU.BA.B.L = ADD8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 8;

		OP(08) // ADC A, A
			MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU.BA.B.L);
			return 8;
		OP(09) // ADC A, B
			MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(0A) // ADC A, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(0B) // ADC A, [HL]
			MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 8;
		OP(0C) // ADC A, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 12;
		OP(0D) // ADC A, [#nnnn]
			I16 = Fetch16();
			MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16));
			return 16;
		OP(0E) // ADC A, [X]
			MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 8;
		OP(0F) // ADC A, [Y]
			MinxCPU.BA.B.L = ADC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 8;

		OP(10) // SUB A, A
			MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU.BA.B.L);
			return 8;
		OP(11) // SUB A, B
			MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(12) // SUB A, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(13) // SUB A, [HL]
			MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 8;
		OP(14) // SUB A, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 12;
		OP(15) // SUB A, [#nnnn]
			I16 = Fetch16();
			MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16));
			return 16;
		OP(16) // SUB A, [X]
			MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 8;
		OP(17) // SUB A, [Y]
			MinxCPU.BA.B.L = SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 8;

		OP(18) // SBC A, A
			MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU.BA.B.L);
			return 8;
		OP(19) // SBC A, B
			MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(1A) // SBC A, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(1B) // SBC A, [HL]
			MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 8;
		OP(1C) // SBC A, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 12;
		OP(1D) // SBC A, [#nnnn]
			I16 = Fetch16();
			MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16));
			return 16;
		OP(1E) // SBC A, [X]
			MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 8;
		OP(1F) // SBC A, [Y]
			MinxCPU.BA.B.L = SBC8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 8;

		OP(20) // AND A, A
			MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU.BA.B.L);
			return 8;
		OP(21) // AND A, B
			MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(22) // AND A, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(23) // AND A, [HL]
			MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 8;
		OP(24) // AND A, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 12;
		OP(25) // AND A, [#nnnn]
			I16 = Fetch16();
			MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16));
			return 16;
		OP(26) // AND A, [X]
			MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 8;
		OP(27) // AND A, [Y]
			MinxCPU.BA.B.L = AND8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 8;

		OP(28) // OR A, A
			MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU.BA.B.L);
			return 8;
		OP(29) // OR A, B
			MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(2A) // OR A, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(2B) // OR A, [HL]
			MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 8;
		OP(2C) // OR A, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 12;
		OP(2D) // OR A, [#nnnn]
			I16 = Fetch16();
			MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16));
			return 16;
		OP(2E) // OR A, [X]
			MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 8;
		OP(2F) // OR A, [Y]
			MinxCPU.BA.B.L = OR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 8;

		OP(30) // CMP A, A
			SUB8(MinxCPU.BA.B.L, MinxCPU.BA.B.L);
			return 8;
		OP(31) // CMP A, B
			SUB8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(32) // CMP A, #nn
			I8A = Fetch8();
			SUB8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(33) // CMP A, [HL]
			SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 8;
		OP(34) // CMP A, [N+#nn]
			I8A = Fetch8();
			SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 12;
		OP(35) // CMP A, [#nnnn]
			I16 = Fetch16();
			SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16));
			return 16;
		OP(36) // CMP A, [X]
			SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 8;
		OP(37) // CMP A, [Y]
			SUB8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 8;

		OP(38) // XOR A, A
			MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU.BA.B.L);
			return 8;
		OP(39) // XOR A, B
			MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(3A) // XOR A, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(3B) // XOR A, [HL]
			MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 8;
		OP(3C) // XOR A, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 12;
		OP(3D) // XOR A, [#nnnn]
			I16 = Fetch16();
			MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16));
			return 16;
		OP(3E) // XOR A, [X]
			MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 8;
		OP(3F) // XOR A, [Y]
			MinxCPU.BA.B.L = XOR8(MinxCPU.BA.B.L, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 8;

		OP(40) // MOV A, A
			return 4;
		OP(41) // MOV A, B
			MinxCPU.BA.B.L = MinxCPU.BA.B.H;
			return 4;
		OP(42) // MOV A, L
			MinxCPU.BA.B.L = MinxCPU.HL.B.L;
			return 4;
		OP(43) // MOV A, H
			MinxCPU.BA.B.L = MinxCPU.HL.B.H;
			return 4;
		OP(44) // MOV A, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.L = MinxCPU_OnRead(1, MinxCPU.N.D + I8A);
			return 12;
		OP(45) // MOV A, [HL]
			MinxCPU.BA.B.L = MinxCPU_OnRead(1, MinxCPU.HL.D);
			return 8;
		OP(46) // MOV A, [X]
			MinxCPU.BA.B.L = MinxCPU_OnRead(1, MinxCPU.X.D);
			return 8;
		OP(47) // MOV A, [Y]
			MinxCPU.BA.B.L = MinxCPU_OnRead(1, MinxCPU.Y.D);
			return 8;

		OP(48) // MOV B, A
			MinxCPU.BA.B.H = MinxCPU.BA.B.L;
			return 4;
		OP(49) // MOV B, B
			return 4;
		OP(4A) // MOV B, L
			MinxCPU.BA.B.H = MinxCPU.HL.B.L;
			return 4;
		OP(4B) // MOV B, H
			MinxCPU.BA.B.H = MinxCPU.HL.B.H;
			return 4;
		OP(4C) // MOV B, [N+#nn]
			I8A = Fetch8();
			MinxCPU.BA.B.H = MinxCPU_OnRead(1, MinxCPU.N.D + I8A);
			return 12;
		OP(4D) // MOV B, [HL]
			MinxCPU.BA.B.H = MinxCPU_OnRead(1, MinxCPU.HL.D);
			return 8;
		OP(4E) // MOV B, [X]
			MinxCPU.BA.B.H = MinxCPU_OnRead(1, MinxCPU.X.D);
			return 8;
		OP(4F) // MOV B, [Y]
			MinxCPU.BA.B.H = MinxCPU_OnRead(1, MinxCPU.Y.D);
			return 8;

		OP(50) // MOV L, A
			MinxCPU.HL.B.L = MinxCPU.BA.B.L;
			return 4;
		OP(51) // MOV L, B
			MinxCPU.HL.B.L = MinxCPU.BA.B.H;
			return 4;
		OP(52) // MOV L, L
			return 4;
		OP(53) // MOV L, H
			MinxCPU.HL.B.L = MinxCPU.HL.B.H;
			return 4;
		OP(54) // MOV L, [N+#nn]
			I8A = Fetch8();
			MinxCPU.HL.B.L = MinxCPU_OnRead(1, MinxCPU.N.D + I8A);
			return 12;
		OP(55) // MOV L, [HL]
			MinxCPU.HL.B.L = MinxCPU_OnRead(1, MinxCPU.HL.D);
			return 8;
		OP(56) // MOV L, [X]
			MinxCPU.HL.B.L = MinxCPU_OnRead(1, MinxCPU.X.D);
			return 8;
		OP(57) // MOV L, [Y]
			MinxCPU.HL.B.L = MinxCPU_OnRead(1, MinxCPU.Y.D);
			return 8;

		OP(58) // MOV H, A
			MinxCPU.HL.B.H = MinxCPU.BA.B.L;
			return 4;
		OP(59) // MOV H, B
			MinxCPU.HL.B.H = MinxCPU.BA.B.H;
			return 4;
		OP(5A) // MOV H, L
			MinxCPU.HL.B.H = MinxCPU.HL.B.L;
			return 4;
		OP(5B) // MOV H, H
			return 4;
		OP(5C) // MOV H, [N+#nn]
			I8A = Fetch8();
			MinxCPU.HL.B.H = MinxCPU_OnRead(1, MinxCPU.N.D + I8A);
			return 12;
		OP(5D) // MOV H, [HL]
			MinxCPU.HL.B.H = MinxCPU_OnRead(1, MinxCPU.HL.D);
			return 8;
		OP(5E) // MOV H, [X]
			MinxCPU.HL.B.H = MinxCPU_OnRead(1, MinxCPU.X.D);
			return 8;
		OP(5F) // MOV H, [Y]
			MinxCPU.HL.B.H = MinxCPU_OnRead(1, MinxCPU.Y.D);
			return 8;

		OP(60) // MOV [X], A
			MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU.BA.B.L);
			return 8;
		OP(61) // MOV [X], B
			MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU.BA.B.H);
			return 8;
		OP(62) // MOV [X], L
			MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU.HL.B.L);
			return 8;
		OP(63) // MOV [X], H
			MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU.HL.B.H);
			return 8;
		OP(64) // MOV [X], [N+#nn]
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 16;
		OP(65) // MOV [X], [HL]
			MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 12;
		OP(66) // MOV [X], [X]
			MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 12;
		OP(67) // MOV [X], [Y]
			MinxCPU_OnWrite(1, MinxCPU.X.D, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 12;

		OP(68) // MOV [HL], A
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.BA.B.L);
			return 8;
		OP(69) // MOV [HL], B
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.BA.B.H);
			return 8;
		OP(6A) // MOV [HL], L
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.HL.B.L);
			return 8;
		OP(6B) // MOV [HL], H
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.HL.B.H);
			return 8;
		OP(6C) // MOV [HL], [N+#nn]
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 16;
		OP(6D) // MOV [HL], [HL]
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 12;
		OP(6E) // MOV [HL], [X]
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 12;
		OP(6F) // MOV [HL], [Y]
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 12;

		OP(70) // MOV [Y], A
			MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU.BA.B.L);
			return 8;
		OP(71) // MOV [Y], B
			MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU.BA.B.H);
			return 8;
		OP(72) // MOV [Y], L
			MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU.HL.B.L);
			return 8;
		OP(73) // MOV [Y], H
			MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU.HL.B.H);
			return 8;
		OP(74) // MOV [Y], [N+#nn]
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU_OnRead(1, MinxCPU.N.D + I8A));
			return 16;
		OP(75) // MOV [Y], [HL]
			MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 12;
		OP(76) // MOV [Y], [X]
			MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 12;
		OP(77) // MOV [Y], [Y]
			MinxCPU_OnWrite(1, MinxCPU.Y.D, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 12;

		OP(78) // MOV [N+#nn], A
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU.BA.B.L);
			return 8;
		OP(79) // MOV [N+#nn], B
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU.BA.B.H);
			return 8;
		OP(7A) // MOV [N+#nn], L
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU.HL.B.L);
			return 8;
		OP(7B) // MOV [N+#nn], H
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU.HL.B.H);
			return 8;
		OP(7C) // NOTHING #nn
			I8A = Fetch8();
			return 64;
		OP(7D) // MOV [N+#nn], [HL]
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU_OnRead(1, MinxCPU.HL.D));
			return 16;
		OP(7E) // MOV [N+#nn], [X]
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU_OnRead(1, MinxCPU.X.D));
			return 16;
		OP(7F) // MOV [N+#nn], [Y]
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, MinxCPU_OnRead(1, MinxCPU.Y.D));
			return 16;

		OP(80) // INC A
			MinxCPU.BA.B.L = INC8(MinxCPU.BA.B.L);
			return 8;
		OP(81) // INC B
			MinxCPU.BA.B.H = INC8(MinxCPU.BA.B.H);
			return 8;
		OP(82) // INC L
			MinxCPU.HL.B.L = INC8(MinxCPU.HL.B.L);
			return 8;
		OP(83) // INC H
			MinxCPU.HL.B.H = INC8(MinxCPU.HL.B.H);
			return 8;
		OP(84) // INC N
			MinxCPU.N.B.H = INC8(MinxCPU.N.B.H);
			return 8;
		OP(85) // INC [N+#nn]
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, INC8(MinxCPU_OnRead(1, MinxCPU.N.D + I8A)));
			return 16;
		OP(86) // INC [HL]
			MinxCPU_OnWrite(1, MinxCPU.HL.D, INC8(MinxCPU_OnRead(1, MinxCPU.HL.D)));
			return 12;
		OP(87) // INC SP
			MinxCPU.SP.W.L = INC16(MinxCPU.SP.W.L);
			return 8;

		OP(88) // DEC A
			MinxCPU.BA.B.L = DEC8(MinxCPU.BA.B.L);
			return 8;
		OP(89) // DEC B
			MinxCPU.BA.B.H = DEC8(MinxCPU.BA.B.H);
			return 8;
		OP(8A) // DEC L
			MinxCPU.HL.B.L = DEC8(MinxCPU.HL.B.L);
			return 8;
		OP(8B) // DEC H
			MinxCPU.HL.B.H = DEC8(MinxCPU.HL.B.H);
			return 8;
		OP(8C) // DEC N
			MinxCPU.N.B.H = DEC8(MinxCPU.N.B.H);
			return 8;
		OP(8D) // DEC [N+#nn]
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, DEC8(MinxCPU_OnRead(1, MinxCPU.N.D + I8A)));
			return 16;
		OP(8E) // DEC [HL]
			MinxCPU_OnWrite(1, MinxCPU.HL.D, DEC8(MinxCPU_OnRead(1, MinxCPU.HL.D)));
			return 12;
		OP(8F) // DEC SP
			MinxCPU.SP.W.L = DEC16(MinxCPU.SP.W.L);
			return 8;

		OP(90) // INC BA
			MinxCPU.BA.W.L = INC16(MinxCPU.BA.W.L);
			return 8;
		OP(91) // INC HL
			MinxCPU.HL.W.L = INC16(MinxCPU.HL.W.L);
			return 8;
		OP(92) // INC X
			MinxCPU.X.W.L = INC16(MinxCPU.X.W.L);
			return 8;
		OP(93) // INC Y
			MinxCPU.Y.W.L = INC16(MinxCPU.Y.W.L);
			return 8;

		OP(94) // TST A, B
			AND8(MinxCPU.BA.B.L, MinxCPU.BA.B.H);
			return 8;
		OP(95) // TST [HL], #nn
			I8A = Fetch8();
			AND8(MinxCPU_OnRead(1, MinxCPU.HL.D), I8A);
			return 12;
		OP(96) // TST A, #nn
			I8A = Fetch8();
			AND8(MinxCPU.BA.B.L, I8A);
			return 8;
		OP(97) // TST B, #nn
			I8A = Fetch8();
			AND8(MinxCPU.BA.B.H, I8A);
			return 8;

		OP(98) // DEC BA
			MinxCPU.BA.W.L = DEC16(MinxCPU.BA.W.L);
			return 8;
		OP(99) // DEC HL
			MinxCPU.HL.W.L = DEC16(MinxCPU.HL.W.L);
			return 8;
		OP(9A) // DEC X
			MinxCPU.X.W.L = DEC16(MinxCPU.X.W.L);
			return 8;
		OP(9B) // DEC Y
			MinxCPU.Y.W.L = DEC16(MinxCPU.Y.W.L);
			return 8;

		OP(9C) // AND F, #nn
			I8A = Fetch8();
			MinxCPU.F = MinxCPU.F & I8A;
			MinxCPU_OnIRQHandle(MinxCPU.F, MinxCPU.Shift_U);
			return 12;
		OP(9D) // OR F, #nn
			I8A = Fetch8();
			MinxCPU.F = MinxCPU.F | I8A;
			MinxCPU_OnIRQHandle(MinxCPU.F, MinxCPU.Shift_U);
			return 12;
		OP(9E) // XOR F, #nn
			I8A = Fetch8();
			MinxCPU.F = MinxCPU.F ^ I8A;
			MinxCPU_OnIRQHandle(MinxCPU.F, MinxCPU.Shift_U);
			return 12;
		OP(9F) // MOV F, #nn
			I8A = Fetch8();
			MinxCPU.F = I8A;
			MinxCPU_OnIRQHandle(MinxCPU.F, MinxCPU.Shift_U);
			return 12;

		OP(A0) // PUSH BA
			PUSH(MinxCPU.BA.B.H);
			PUSH(MinxCPU.BA.B.L);
			return 16;
		OP(A1) // PUSH HL
			PUSH(MinxCPU.HL.B.H);
			PUSH(MinxCPU.HL.B.L);
			return 16;
		OP(A2) // PUSH X
			PUSH(MinxCPU.X.B.H);
			PUSH(MinxCPU.X.B.L);
			return 16;
		OP(A3) // PUSH Y
			PUSH(MinxCPU.Y.B.H);
			PUSH(MinxCPU.Y.B.L);
			return 16;
		OP(A4) // PUSH N
			PUSH(MinxCPU.N.B.H);
			return 12;
		OP(A5) // PUSH I
			PUSH(MinxCPU.HL.B.I);
			return 12;
		OP(A6) // PUSHX
			PUSH(MinxCPU.X.B.I);
			PUSH(MinxCPU.Y.B.I);
			return 16;
		OP(A7) // PUSH F
			PUSH(MinxCPU.F);
			return 12;

		OP(A8) // POP BA
			MinxCPU.BA.B.L = POP();
			MinxCPU.BA.B.H = POP();
			return 12;
		OP(A9) // POP HL
			MinxCPU.HL.B.L = POP();
			MinxCPU.HL.B.H = POP();
			return 12;
		OP(AA) // POP X
			MinxCPU.X.B.L = POP();
			MinxCPU.X.B.H = POP();
			return 12;
		OP(AB) // POP Y
			MinxCPU.Y.B.L = POP();
			MinxCPU.Y.B.H = POP();
			return 12;
		OP(AC) // POP N
			MinxCPU.N.B.H = POP();
			return 8;
		OP(AD) // POP I
			MinxCPU.HL.B.I = POP();
			MinxCPU.N.B.I = MinxCPU.HL.B.I;
			return 8;
		OP(AE) // POPX
			MinxCPU.Y.B.I = POP();
			MinxCPU.X.B.I = POP();
			return 12;
		OP(AF) // POP F
			MinxCPU.F = POP();
			MinxCPU_OnIRQHandle(MinxCPU.F, MinxCPU.Shift_U);
			return 8;

		OP(B0) // MOV A, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.L = I8A;
			return 8;
		OP(B1) // MOV B, #nn
			I8A = Fetch8();
			MinxCPU.BA.B.H = I8A;
			return 8;
		OP(B2) // MOV L, #nn
			I8A = Fetch8();
			MinxCPU.HL.B.L = I8A;
			return 8;
		OP(B3) // MOV H, #nn
			I8A = Fetch8();
			MinxCPU.HL.B.H = I8A;
			return 8;
		OP(B4) // MOV N, #nn
			I8A = Fetch8();
			MinxCPU.N.B.H = I8A;
			return 8;
		OP(B5) // MOV [HL], #nn
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.HL.D, I8A);
			return 12;
		OP(B6) // MOV [X], #nn
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.X.D, I8A);
			return 12;
		OP(B7) // MOV [Y], #nn
			I8A = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.Y.D, I8A);
			return 12;

		OP(B8) // MOV BA, [#nnnn]
			I16 = Fetch16();
			MinxCPU.BA.B.L = MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16++);
			MinxCPU.BA.B.H = MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16);
			return 20;
		OP(B9) // MOV HL, [#nnnn]
			I16 = Fetch16();
			MinxCPU.HL.B.L = MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16++);
			MinxCPU.HL.B.H = MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16);
			return 20;
		OP(BA) // MOV X, [#nnnn]
			I16 = Fetch16();
			MinxCPU.X.B.L = MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16++);
			MinxCPU.X.B.H = MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16);
			return 20;
		OP(BB) // MOV Y, [#nnnn]
			I16 = Fetch16();
			MinxCPU.Y.B.L = MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16++);
			MinxCPU.Y.B.H = MinxCPU_OnRead(1, (MinxCPU.HL.B.I << 16) | I16);
			return 20;

		OP(BC) // MOV [#nnnn], BA
			I16 = Fetch16();
			MinxCPU_OnWrite(1, (MinxCPU.HL.B.I << 16) | I16++, MinxCPU.BA.B.L);
			MinxCPU_OnWrite(1, (MinxCPU.HL.B.I << 16) | I16, MinxCPU.BA.B.H);
			return 20;
		OP(BD) // MOV [#nnnn], HL
			I16 = Fetch16();
			MinxCPU_OnWrite(1, (MinxCPU.HL.B.I << 16) | I16++, MinxCPU.HL.B.L);
			MinxCPU_OnWrite(1, (MinxCPU.HL.B.I << 16) | I16, MinxCPU.HL.B.H);
			return 20;
		OP(BE) // MOV [#nnnn], X
			I16 = Fetch16();
			MinxCPU_OnWrite(1, (MinxCPU.HL.B.I << 16) | I16++, MinxCPU.X.B.L);
			MinxCPU_OnWrite(1, (MinxCPU.HL.B.I << 16) | I16, MinxCPU.X.B.H);
			return 20;
		OP(BF) // MOV [#nnnn], Y
			I16 = Fetch16();
			MinxCPU_OnWrite(1, (MinxCPU.HL.B.I << 16) | I16++, MinxCPU.Y.B.L);
			MinxCPU_OnWrite(1, (MinxCPU.HL.B.I << 16) | I16, MinxCPU.Y.B.H);
			return 20;

		OP(C0) // ADD BA, #nnnn
			I16 = Fetch16();
			MinxCPU.BA.W.L = ADD16(MinxCPU.BA.W.L, I16);
			return 12;
		OP(C1) // ADD HL, #nnnn
			I16 = Fetch16();
			MinxCPU.HL.W.L = ADD16(MinxCPU.HL.W.L, I16);
			return 12;
		OP(C2) // ADD X, #nnnn
			I16 = Fetch16();
			MinxCPU.X.W.L = ADD16(MinxCPU.X.W.L, I16);
			return 12;
		OP(C3) // ADD Y, #nnnn
			I16 = Fetch16();
			MinxCPU.Y.W.L = ADD16(MinxCPU.Y.W.L, I16);
			return 12;

		OP(C4) // MOV BA, #nnnn
			I16 = Fetch16();
			MinxCPU.BA.W.L = I16;
			return 12;
		OP(C5) // MOV HL, #nnnn
			I16 = Fetch16();
			MinxCPU.HL.W.L = I16;
			return 12;
		OP(C6) // MOV X, #nnnn
			I16 = Fetch16();
			MinxCPU.X.W.L = I16;
			return 12;
		OP(C7) // MOV Y, #nnnn
			I16 = Fetch16();
			MinxCPU.Y.W.L = I16;
			return 12;

		OP(C8) // XCHG BA, HL
			I16 = MinxCPU.HL.W.L;
			MinxCPU.HL.W.L = MinxCPU.BA.W.L;
			MinxCPU.BA.W.L = I16;
			return 12;
		OP(C9) // XCHG BA, X
			I16 = MinxCPU.X.W.L;
			MinxCPU.X.W.L = MinxCPU.BA.W.L;
			MinxCPU.BA.W.L = I16;
			return 12;
		OP(CA) // XCHG BA, Y
			I16 = MinxCPU.Y.W.L;
			MinxCPU.Y.W.L = MinxCPU.BA.W.L;
			MinxCPU.BA.W.L = I16;
			return 12;
		OP(CB) // XCHG BA, SP
			I16 = MinxCPU.SP.W.L;
			MinxCPU.SP.W.L = MinxCPU.BA.W.L;
			MinxCPU.BA.W.L = I16;
			return 12;

		OP(CC) // XCHG A, B
			I8A = MinxCPU.BA.B.H;
			MinxCPU.BA.B.H = MinxCPU.BA.B.L;
			MinxCPU.BA.B.L = I8A;
			return 8;
		OP(CD) // XCHG A, [HL]
			I8A = MinxCPU_OnRead(1, MinxCPU.HL.D);
			MinxCPU_OnWrite(1, MinxCPU.HL.D, MinxCPU.BA.B.L);
			MinxCPU.BA.B.L = I8A;
			return 12;

		OP(CE) // Expand 0
			return MinxCPU_ExecCE();

		OP(CF) // Expand 1
			return MinxCPU_ExecCF();

		OP(D0) // SUB BA, #nnnn
			I16 = Fetch16();
			MinxCPU.BA.W.L = SUB16(MinxCPU.BA.W.L, I16);
			return 12;
		OP(D1) // SUB HL, #nnnn
			I16 = Fetch16();
			MinxCPU.HL.W.L = SUB16(MinxCPU.HL.W.L, I16);
			return 12;
		OP(D2) // SUB X, #nnnn
			I16 = Fetch16();
			MinxCPU.X.W.L = SUB16(MinxCPU.X.W.L, I16);
			return 12;
		OP(D3) // SUB Y, #nnnn
			I16 = Fetch16();
			MinxCPU.Y.W.L = SUB16(MinxCPU.Y.W.L, I16);
			return 12;

		OP(D4) // CMP BA, #nnnn
			I16 = Fetch16();
			SUB16(MinxCPU.BA.W.L, I16);
			return 12;
		OP(D5) // CMP HL, #nnnn
			I16 = Fetch16();
			SUB16(MinxCPU.HL.W.L, I16);
			return 12;
		OP(D6) // CMP X, #nnnn
			I16 = Fetch16();
			SUB16(MinxCPU.X.W.L, I16);
			return 12;
		OP(D7) // CMP Y, #nnnn
			I16 = Fetch16();
			SUB16(MinxCPU.Y.W.L, I16);
			return 12;

		OP(D8) // AND [N+#nn], #nn
			I8A = Fetch8();
			I8B = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, AND8(MinxCPU_OnRead(1, MinxCPU.N.D + I8A), I8B));
			return 20;
		OP(D9) // OR [N+#nn], #nn
			I8A = Fetch8();
			I8B = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, OR8(MinxCPU_OnRead(1, MinxCPU.N.D + I8A), I8B));
			return 20;
		OP(DA) // XOR [N+#nn], #nn
			I8A = Fetch8();
			I8B = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, XOR8(MinxCPU_OnRead(1, MinxCPU.N.D + I8A), I8B));
			return 20;
		OP(DB) // CMP [N+#nn], #nn
			I8A = Fetch8();
			I8B = Fetch8();
			SUB8(MinxCPU_OnRead(1, MinxCPU.N.D + I8A), I8B);
			return 16;
		OP(DC) // TST [N+#nn], #nn
			I8A = Fetch8();
			I8B = Fetch8();
			AND8(MinxCPU_OnRead(1, MinxCPU.N.D + I8A), I8B);
			return 16;
		OP(DD) // MOV [N+#nn], #nn
			I8A = Fetch8();
			I8B = Fetch8();
			MinxCPU_OnWrite(1, MinxCPU.N.D + I8A, I8B);
			return 16;

		OP(DE) // PACK
			MinxCPU.BA.B.L = (MinxCPU.BA.B.L & 0x0F) | (MinxCPU.BA.B.H << 4);
			return 8;

		OP(DF) // UNPACK
			MinxCPU.BA.B.H = (MinxCPU.BA.B.L >> 4);
			MinxCPU.BA.B.L = MinxCPU.BA.B.L & 0x0F;
			return 8;

		OP(E0) // CALLC #ss
			I8A = Fetch8();
			if (MinxCPU.F & MINX_FLAG_CARRY) {
				CALLS(S8_TO_16(I8A));
				return 20;
			}
			return 8;
		OP(E1) // CALLNC #ss
			I8A = Fetch8();
			if (!(MinxCPU.F & MINX_FLAG_CARRY)) {
				CALLS(S8_TO_16(I8A));
				return 20;
			}
			return 8;
		OP(E2) // CALLZ #ss
			I8A = Fetch8();
			if (MinxCPU.F & MINX_FLAG_ZERO) {
				CALLS(S8_TO_16(I8A));
				return 20;
			}
			return 8;
		OP(E3) // CALLNZ #ss
			I8A = Fetch8();
			if (!(MinxCPU.F & MINX_FLAG_ZERO)) {
				CALLS(S8_TO_16(I8A));
				return 20;
			}
			return 8;

		OP(E4) // JC #ss
			I8A = Fetch8();
			if (MinxCPU.F & MINX_FLAG_CARRY) {
				JMPS(S8_TO_16(I8A));
			}
			return 8;
		OP(E5) // JNC #ss
			I8A = Fetch8();
			if (!(MinxCPU.F & MINX_FLAG_CARRY)) {
				JMPS(S8_TO_16(I8A));
			}
			return 8;
		OP(E6) // JZ #ss
			I8A = Fetch8();
			if (MinxCPU.F & MINX_FLAG_ZERO) {
				JMPS(S8_TO_16(I8A));
			}
			return 8;
		OP(E7) // JNZ #ss
			I8A = Fetch8();
			if (!(MinxCPU.F & MINX_FLAG_ZERO)) {
				JMPS(S8_TO_16(I8A));
			}
			return 8;

		OP(E8) // CALLC #ssss
			I16 = Fetch16();
			if (MinxCPU.F & MINX_FLAG_CARRY) {
				CALLS(I16);
				return 24;
			}
			return 12;
		OP(E9) // CALLNC #ssss
			I16 = Fetch16();
			if (!(MinxCPU.F & MINX_FLAG_CARRY)) {
				CALLS(I16);
				return 24;
			}
			return 12;
		OP(EA) // CALLZ #ssss
			I16 = Fetch16();
			if (MinxCPU.F & MINX_FLAG_ZERO) {
				CALLS(I16);
				return 24;
			}
			return 12;
		OP(EB) // CALLNZ #ssss
			I16 = Fetch16();
			if (!(MinxCPU.F & MINX_FLAG_ZERO)) {
				CALLS(I16);
				return 24;
			}
			return 12;

		OP(EC) // JC #ssss
			I16 = Fetch16();
			if (MinxCPU.F & MINX_FLAG_CARRY) {
				JMPS(I16);
			}
			return 12;
		OP(ED) // JNC #ssss
			I16 = Fetch16();
			if (!(MinxCPU.F & MINX_FLAG_CARRY)) {
				JMPS(I16);
			}
			return 12;
		OP(EE) // JZ #ssss
			I16 = Fetch16();
			if (MinxCPU.F & MINX_FLAG_ZERO) {
				JMPS(I16);
			}
			return 12;
		OP(EF) // JNZ #ssss
			I16 = Fetch16();
			if (!(MinxCPU.F & MINX_FLAG_ZERO)) {
				JMPS(I16);
			}
			return 12;

		OP(F0) // CALL #ss
			I8A = Fetch8();
			CALLS(S8_TO_16(I8A));
			return 20;
		OP(F1) // JMP #ss
			I8A = Fetch8();
			JMPS(S8_TO_16(I8A));
			return 8;
		OP(F2) // CALL #ssss
			I16 = Fetch16();
			CALLS(I16);
			return 24;
		OP(F3) // JMP #ssss
			I16 = Fetch16();
			JMPS(I16);
			return 12;

		OP(F4) // JMP HL
			JMPU(MinxCPU.HL.W.L);
			return 8;

		OP(F5) // JDBNZ #ss
			I8A = Fetch8();
			JDBNZ(S8_TO_16(I8A));
			return 16;

		OP(F6) // SWAP A
			MinxCPU.BA.B.L = SWAP(MinxCPU.BA.B.L);
			return 8;
		OP(F7) // SWAP [HL]
			MinxCPU_OnWrite(1, MinxCPU.HL.D, SWAP(MinxCPU_OnRead(1, MinxCPU.HL.D)));
			return 12;

		OP(F8) // RET
			RET();
			return 16;
		OP(F9) // RETI
			RETI();
			return 16;
		OP(FA) // RETSKIP
			RET();
			MinxCPU.PC.W.L = MinxCPU.PC.W.L + 2;
			return 16;

		OP(FB) // CALL [#nnnn]
			I16 = Fetch16();
			CALLX(I16);
			return 20;
		OP(FC) // CINT #nn
			I16 = Fetch8();
			CALLI(I16);
			return 20;
		OP(FD) // JINT #nn
			I16 = Fetch8();
			JMPI(I16);
			return 8;

		OP(FE) // CRASH
			MinxCPU_OnException(EXCEPTION_CRASH_INSTRUCTION, 0xFE);
			return 4;

		OP(FF) // NOP
			return 8;

		OP_DEFAULT
			MinxCPU_OnException(EXCEPTION_UNKNOWN_INSTRUCTION, MinxCPU.IR);
			return 4;
#if !(defined(POKEMINI_COMPUTED_GOTO) && defined(__GNUC__))
	}
#endif
#undef OP
#undef OP_DEFAULT
}
