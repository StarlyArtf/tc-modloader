#pragma once

#include <cstdint>

namespace tc::rv32_intrinsic {

struct Outputs {
    uint32_t pcNext = 0;
    uint32_t regWriteData = 0;
    uint32_t memoryAddress = 0;
    uint32_t memoryWriteData = 0;
    uint8_t rs1Address = 0;
    uint8_t rs2Address = 0;
    uint8_t regWriteAddress = 0;
    uint8_t memoryByteEnable = 0;
    bool regWrite = false;
    bool memoryWrite = false;
    bool illegalInstruction = false;
    bool ecall = false;
    bool ebreak = false;
};

inline uint32_t signExtend(uint32_t value, uint32_t bits) {
    const uint32_t sign = uint32_t(1) << (bits - 1);
    return (value ^ sign) - sign;
}

/* Exact two-state evaluator for the stateless rv32i.v/rv32i_flat.v core used
   by the TC-Verilog example.  The source compiler enables this only for one
   of the two byte-exact, audited RTL files; it is not a generic Verilog
   substitution. */
inline Outputs evaluate(bool bigEndian, uint32_t pc, uint32_t instr,
                        uint32_t rs1Data, uint32_t rs2Data,
                        uint32_t memoryReadData) {
    constexpr uint32_t OP_LUI = 0x37, OP_AUIPC = 0x17, OP_JAL = 0x6f,
                       OP_JALR = 0x67, OP_BRANCH = 0x63, OP_LOAD = 0x03,
                       OP_STORE = 0x23, OP_IMM = 0x13, OP_REG = 0x33,
                       OP_FENCE = 0x0f, OP_SYSTEM = 0x73;
    enum Alu : uint8_t { Add, Sub, Sll, Slt, Sltu, Xor, Srl, Sra, Or, And };
    enum Writeback : uint8_t { WbAlu, WbMemory, WbPc4, WbImmediate };

    Outputs out;
    const uint32_t opcode = instr & 0x7f;
    const uint32_t rd = (instr >> 7) & 31;
    const uint32_t funct3 = (instr >> 12) & 7;
    const uint32_t rs1 = (instr >> 15) & 31;
    const uint32_t rs2 = (instr >> 20) & 31;
    const uint32_t funct7 = instr >> 25;
    out.rs1Address = static_cast<uint8_t>(rs1 << 2);
    out.rs2Address = static_cast<uint8_t>(rs2 << 2);
    out.regWriteAddress = static_cast<uint8_t>(rd << 2);

    const uint32_t immI = signExtend(instr >> 20, 12);
    const uint32_t immS = signExtend(((instr >> 25) << 5) | ((instr >> 7) & 31), 12);
    const uint32_t immB = signExtend(((instr >> 31) << 12) |
        (((instr >> 7) & 1) << 11) | (((instr >> 25) & 63) << 5) |
        (((instr >> 8) & 15) << 1), 13);
    const uint32_t immU = instr & 0xfffff000u;
    const uint32_t immJ = signExtend(((instr >> 31) << 20) |
        (((instr >> 12) & 0xff) << 12) | (((instr >> 20) & 1) << 11) |
        (((instr >> 21) & 0x3ff) << 1), 21);

    Alu alu = Add;
    Writeback writeback = WbAlu;
    bool operandAPc = false, operandBImmediate = false, immediateS = false,
         immediateU = false, rfWrite = false, dataMemoryWrite = false,
         pcSelect = false, jal = false, jalr = false;

    const bool signedLess = static_cast<int32_t>(rs1Data) < static_cast<int32_t>(rs2Data);
    bool branchTaken = false;
    switch (funct3) {
    case 0: branchTaken = rs1Data == rs2Data; break;
    case 1: branchTaken = rs1Data != rs2Data; break;
    case 4: branchTaken = signedLess; break;
    case 5: branchTaken = !signedLess; break;
    case 6: branchTaken = rs1Data < rs2Data; break;
    case 7: branchTaken = rs1Data >= rs2Data; break;
    }

    switch (opcode) {
    case OP_LUI:
        writeback = WbImmediate; rfWrite = true; break;
    case OP_AUIPC:
        operandAPc = true; operandBImmediate = true; immediateU = true;
        rfWrite = true; break;
    case OP_JAL:
        jal = true; pcSelect = true; writeback = WbPc4; rfWrite = true; break;
    case OP_JALR:
        if (funct3 == 0) {
            jalr = true; pcSelect = true; writeback = WbPc4; rfWrite = true;
        } else out.illegalInstruction = true;
        break;
    case OP_BRANCH:
        if (funct3 == 0 || funct3 == 1 || funct3 == 4 || funct3 == 5 ||
            funct3 == 6 || funct3 == 7)
            pcSelect = branchTaken;
        else out.illegalInstruction = true;
        break;
    case OP_LOAD:
        if (funct3 == 0 || funct3 == 1 || funct3 == 2 || funct3 == 4 || funct3 == 5) {
            operandBImmediate = true; writeback = WbMemory; rfWrite = true;
        } else out.illegalInstruction = true;
        break;
    case OP_STORE:
        if (funct3 <= 2) {
            operandBImmediate = true; immediateS = true; dataMemoryWrite = true;
        } else out.illegalInstruction = true;
        break;
    case OP_IMM:
        operandBImmediate = true; rfWrite = true;
        switch (funct3) {
        case 0: alu = Add; break;
        case 2: alu = Slt; break;
        case 3: alu = Sltu; break;
        case 4: alu = Xor; break;
        case 6: alu = Or; break;
        case 7: alu = And; break;
        case 1:
            if (funct7 == 0) alu = Sll; else out.illegalInstruction = true;
            break;
        case 5:
            if (funct7 == 0) alu = Srl;
            else if (funct7 == 0x20) alu = Sra;
            else out.illegalInstruction = true;
            break;
        default: out.illegalInstruction = true; break;
        }
        break;
    case OP_REG:
        rfWrite = true;
        switch (funct3) {
        case 0:
            if (funct7 == 0) alu = Add;
            else if (funct7 == 0x20) alu = Sub;
            else out.illegalInstruction = true;
            break;
        case 1: if (funct7 == 0) alu = Sll; else out.illegalInstruction = true; break;
        case 2: if (funct7 == 0) alu = Slt; else out.illegalInstruction = true; break;
        case 3: if (funct7 == 0) alu = Sltu; else out.illegalInstruction = true; break;
        case 4: if (funct7 == 0) alu = Xor; else out.illegalInstruction = true; break;
        case 5:
            if (funct7 == 0) alu = Srl;
            else if (funct7 == 0x20) alu = Sra;
            else out.illegalInstruction = true;
            break;
        case 6: if (funct7 == 0) alu = Or; else out.illegalInstruction = true; break;
        case 7: if (funct7 == 0) alu = And; else out.illegalInstruction = true; break;
        }
        break;
    case OP_FENCE:
        if (funct3 != 0 && funct3 != 1) out.illegalInstruction = true;
        break;
    case OP_SYSTEM:
        if (funct3 == 0) {
            const uint32_t code = instr >> 20;
            if (code == 0) out.ecall = true;
            else if (code == 1) out.ebreak = true;
            else out.illegalInstruction = true;
        } else out.illegalInstruction = true;
        break;
    default: out.illegalInstruction = true; break;
    }

    const uint32_t aluImmediate = immediateS ? immS : (immediateU ? immU : immI);
    const uint32_t a = operandAPc ? pc : rs1Data;
    const uint32_t b = operandBImmediate ? aluImmediate : rs2Data;
    const uint32_t shift = b & 31;
    uint32_t aluResult = 0;
    switch (alu) {
    case Add: aluResult = a + b; break;
    case Sub: aluResult = a - b; break;
    case Sll: aluResult = a << shift; break;
    case Slt: aluResult = static_cast<int32_t>(a) < static_cast<int32_t>(b); break;
    case Sltu: aluResult = a < b; break;
    case Xor: aluResult = a ^ b; break;
    case Srl: aluResult = a >> shift; break;
    case Sra: aluResult = static_cast<uint32_t>(static_cast<int32_t>(a) >> shift); break;
    case Or: aluResult = a | b; break;
    case And: aluResult = a & b; break;
    }

    const uint32_t byteOffset = aluResult & 3;
    const uint32_t byteShift = bigEndian ? (3 - byteOffset) * 8 : byteOffset * 8;
    const uint32_t selectedByte = (memoryReadData >> byteShift) & 0xff;
    const uint32_t selectedHalf = bigEndian
        ? ((aluResult & 2) ? memoryReadData & 0xffff : memoryReadData >> 16)
        : ((aluResult & 2) ? memoryReadData >> 16 : memoryReadData & 0xffff);
    uint32_t loadData = 0;
    switch (funct3) {
    case 0: loadData = signExtend(selectedByte, 8); break;
    case 1: loadData = signExtend(selectedHalf, 16); break;
    case 2: loadData = memoryReadData; break;
    case 4: loadData = selectedByte; break;
    case 5: loadData = selectedHalf; break;
    }

    uint32_t storeData = 0;
    uint8_t storeEnable = 0;
    if (funct3 == 0) {
        const uint32_t byte = rs2Data & 0xff;
        storeData = byte * 0x01010101u;
        storeEnable = static_cast<uint8_t>(1u << (bigEndian ? 3 - byteOffset : byteOffset));
    } else if (funct3 == 1) {
        const uint32_t half = rs2Data & 0xffff;
        storeData = half | (half << 16);
        storeEnable = static_cast<uint8_t>(bigEndian
            ? ((aluResult & 2) ? 0x3 : 0xc)
            : ((aluResult & 2) ? 0xc : 0x3));
    } else if (funct3 == 2) {
        storeData = rs2Data; storeEnable = 0xf;
    }

    const uint32_t pcPlus4 = pc + 4;
    const uint32_t pcTarget = jalr ? ((rs1Data + immI) & 0xfffffffeu)
        : (jal ? pc + immJ : pc + immB);
    out.pcNext = pcSelect ? pcTarget : pcPlus4;
    switch (writeback) {
    case WbMemory: out.regWriteData = loadData; break;
    case WbPc4: out.regWriteData = pcPlus4; break;
    case WbImmediate: out.regWriteData = immU; break;
    default: out.regWriteData = aluResult; break;
    }
    out.regWrite = rfWrite && rd != 0;
    out.memoryWrite = dataMemoryWrite;
    out.memoryByteEnable = dataMemoryWrite ? storeEnable : 0;
    out.memoryAddress = aluResult & 0xfffffffcu;
    out.memoryWriteData = storeData;
    return out;
}

inline uint64_t packedInstructionOutputs(const Outputs& out) {
    return uint64_t(out.rs1Address) | (uint64_t(out.rs2Address) << 7) |
        (uint64_t(out.regWrite) << 14) | (uint64_t(out.regWriteAddress) << 15) |
        (uint64_t(out.memoryWrite) << 22) | (uint64_t(out.illegalInstruction) << 23) |
        (uint64_t(out.ecall) << 24) | (uint64_t(out.ebreak) << 25);
}

}  // namespace tc::rv32_intrinsic
