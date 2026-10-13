// worklet2.cpp -- x86-32 encoder. Consumes only enums/integers; no text handling.
#include "worklet2.h"

#include <fstream>

namespace w2 {
namespace {

const size_t kUndefined = static_cast<size_t>(-1);

struct Fixup {
    size_t  pos;        // offset of the 32-bit field in the output
    LabelId label;
    size_t  instr;      // for error reporting
    bool    absolute;   // true: baseAddress + target;  false: rel32 (target - end of field)
};

// ---- byte emission (little-endian, independent of host endianness) --------

inline void put8(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
}
inline void put32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 24));
}
inline void patch32(std::vector<uint8_t>& o, size_t pos, uint32_t v) {
    o[pos + 0] = static_cast<uint8_t>(v);
    o[pos + 1] = static_cast<uint8_t>(v >> 8);
    o[pos + 2] = static_cast<uint8_t>(v >> 16);
    o[pos + 3] = static_cast<uint8_t>(v >> 24);
}

// ---- operand shape predicates ----------------------------------------------

inline uint8_t code(Reg r)                    { return static_cast<uint8_t>(r); }
inline bool isGpr(Reg r)                      { return code(r) < 8; }
inline bool isReg(const Operand& o)           { return o.kind == OperandKind::REG && isGpr(o.reg); }
inline bool isMem(const Operand& o)           { return o.kind == OperandKind::MEM || o.kind == OperandKind::MEMLABEL; }
inline bool isImm(const Operand& o)           { return o.kind == OperandKind::IMM; }
inline bool isRM(const Operand& o)            { return isReg(o) || isMem(o); }
inline bool fitsInt8(int32_t v)               { return v >= -128 && v <= 127; }

// ---- ModRM / SIB / displacement ---------------------------------------------
// Emits the addressing bytes for `rm` (a REG, MEM or MEMLABEL operand).
// `regField` goes in ModRM.reg: either the other register operand, or an
// opcode /digit.
Status emitModRM(std::vector<uint8_t>& out, uint32_t regField, const Operand& rm,
                 std::vector<Fixup>& fixups, size_t idx) {
    if (rm.kind == OperandKind::REG) {                      // mod = 11
        put8(out, 0xC0 | (regField << 3) | code(rm.reg));
        return Status::OK;
    }

    if (rm.kind == OperandKind::MEMLABEL) {                 // [abs32 of label]
        put8(out, (regField << 3) | 0x05);                  // mod=00, rm=101
        Fixup f = { out.size(), rm.label, idx, true };
        fixups.push_back(f);
        put32(out, 0);                                      // patched later
        return Status::OK;
    }

    const MemOperand& m = rm.mem;
    const bool hasBase  = m.base  != Reg::NONE;
    const bool hasIndex = m.index != Reg::NONE;
    if ((hasBase && !isGpr(m.base)) || (hasIndex && !isGpr(m.index))) return Status::BAD_OPERANDS;

    uint32_t scaleBits = 0;
    if (hasIndex) {
        if (m.index == Reg::ESP) return Status::BAD_INDEX_REG;
        switch (m.scale) {
            case 1: scaleBits = 0; break;
            case 2: scaleBits = 1; break;
            case 4: scaleBits = 2; break;
            case 8: scaleBits = 3; break;
            default: return Status::BAD_SCALE;
        }
    }

    if (!hasBase && !hasIndex) {                            // [disp32]
        put8(out, (regField << 3) | 0x05);
        put32(out, static_cast<uint32_t>(m.disp));
        return Status::OK;
    }

    if (!hasBase) {                                         // [index*scale + disp32]
        put8(out, (regField << 3) | 0x04);                  // rm=100 -> SIB follows
        put8(out, (scaleBits << 6) | (code(m.index) << 3) | 0x05);  // base=101 -> none
        put32(out, static_cast<uint32_t>(m.disp));
        return Status::OK;
    }

    // Base present. EBP as base can't use mod=00 (that slot means "no base"),
    // so it always carries an explicit displacement, even a zero one.
    uint32_t mod;
    if (m.disp == 0 && m.base != Reg::EBP) mod = 0;
    else if (fitsInt8(m.disp))             mod = 1;
    else                                   mod = 2;

    // ESP as base (or any index) requires a SIB byte.
    const bool needSib = hasIndex || m.base == Reg::ESP;

    put8(out, (mod << 6) | (regField << 3) | (needSib ? 4u : code(m.base)));
    if (needSib) {
        put8(out, (scaleBits << 6) | ((hasIndex ? code(m.index) : 4u) << 3) | code(m.base));
    }
    if (mod == 1)      put8(out, static_cast<uint32_t>(m.disp));
    else if (mod == 2) put32(out, static_cast<uint32_t>(m.disp));
    return Status::OK;
}

// ---- opcode table for the ALU group (ADD/OR/AND/SUB/XOR/CMP) -----------------
// rmR = "op r/m32, r32", rRm = "op r32, r/m32", ext = /digit for 81 and 83.
// The short "op EAX, imm32" opcode is always rmR + 4.
bool aluForm(Op op, uint8_t& rmR, uint8_t& rRm, uint8_t& ext) {
    switch (op) {
        case Op::ADD: rmR = 0x01; rRm = 0x03; ext = 0; return true;
        case Op::OR:  rmR = 0x09; rRm = 0x0B; ext = 1; return true;
        case Op::AND: rmR = 0x21; rRm = 0x23; ext = 4; return true;
        case Op::SUB: rmR = 0x29; rRm = 0x2B; ext = 5; return true;
        case Op::XOR: rmR = 0x31; rRm = 0x33; ext = 6; return true;
        case Op::CMP: rmR = 0x39; rRm = 0x3B; ext = 7; return true;
        default:      return false;
    }
}

// ---- tables for the added instruction set -------------------------------------
// Opcode of every added instruction that is exactly one byte long.
uint8_t singleByteOpcode(Op op) {
    switch (op) {
        case Op::HLT:    return 0xF4;
        case Op::CLC:    return 0xF8;
        case Op::STC:    return 0xF9;
        case Op::CLD:    return 0xFC;
        case Op::STD:    return 0xFD;
        case Op::CDQ:    return 0x99;
        case Op::LEAVE:  return 0xC9;
        case Op::PUSHAD: return 0x60;
        case Op::POPAD:  return 0x61;
        case Op::PUSHFD: return 0x9C;
        case Op::POPFD:  return 0x9D;
        default:         return 0x90;   // not reachable: callers pre-filter
    }
}

// /digit of the F7 group (NOT/NEG/MUL/DIV/IDIV r/m32).
uint8_t unaryDigit(Op op) {
    switch (op) {
        case Op::NOT:  return 2;
        case Op::NEG:  return 3;
        case Op::MUL:  return 4;
        case Op::DIV:  return 6;
        default:       return 7;        // IDIV
    }
}

// /digit of the C1 group (shift r/m32, imm8).
uint8_t shiftDigit(Op op) {
    switch (op) {
        case Op::SHL: return 4;
        case Op::SHR: return 5;
        default:      return 7;         // SAR
    }
}

// Second opcode byte of a near conditional jump (0F 8x rel32).
uint8_t jccOpcode(Op op) {
    switch (op) {
        case Op::JB:  return 0x82;
        case Op::JAE: return 0x83;
        case Op::JBE: return 0x86;
        case Op::JA:  return 0x87;
        case Op::JS:  return 0x88;
        case Op::JNS: return 0x89;
        case Op::JL:  return 0x8C;
        case Op::JGE: return 0x8D;
        case Op::JLE: return 0x8E;
        default:      return 0x8F;      // JG
    }
}

// ---- one instruction -----------------------------------------------------------
Status encodeOne(const Instruction& in, std::vector<uint8_t>& out,
                 std::vector<Fixup>& fixups, size_t idx) {
    if (in.operandCount > 2) return Status::BAD_OPERANDS;
    const unsigned n = in.operandCount;
    const Operand& a = in.operands[0];
    const Operand& b = in.operands[1];

    switch (in.op) {
        case Op::NOP:
            if (n != 0) return Status::BAD_OPERANDS;
            put8(out, 0x90);
            return Status::OK;

        case Op::RET:
            if (n != 0) return Status::BAD_OPERANDS;
            put8(out, 0xC3);
            return Status::OK;

        case Op::PUSH:
        case Op::POP:
            if (n != 1 || !isReg(a)) return Status::BAD_OPERANDS;
            put8(out, (in.op == Op::PUSH ? 0x50u : 0x58u) + code(a.reg));
            return Status::OK;

        case Op::INC:                                       // 40+rd
        case Op::DEC:                                       // 48+rd
            if (n != 1 || !isReg(a)) return Status::BAD_OPERANDS;
            put8(out, (in.op == Op::INC ? 0x40u : 0x48u) + code(a.reg));
            return Status::OK;

        case Op::JMP:
        case Op::CALL:
        case Op::JE:
        case Op::JNE: {
            if (n != 1 || a.kind != OperandKind::LABEL) return Status::BAD_OPERANDS;
            if (in.op == Op::JMP) {
                put8(out, 0xE9);                            // JMP  rel32
            } else if (in.op == Op::CALL) {
                put8(out, 0xE8);                            // CALL rel32
            } else {
                put8(out, 0x0F);                            // Jcc  rel32
                put8(out, in.op == Op::JE ? 0x84u : 0x85u);
            }
            Fixup f = { out.size(), a.label, idx, false };
            fixups.push_back(f);
            put32(out, 0);                                  // rel32 placeholder
            return Status::OK;
        }

        case Op::MOV: {
            if (n != 2) return Status::BAD_OPERANDS;
            if (isReg(a) && isImm(b)) {                     // MOV r32, imm32   B8+rd
                put8(out, 0xB8u + code(a.reg));
                put32(out, static_cast<uint32_t>(b.imm));
                return Status::OK;
            }
            if (isReg(a) && isRM(b)) {                      // MOV r32, r/m32   8B /r
                put8(out, 0x8B);
                return emitModRM(out, code(a.reg), b, fixups, idx);
            }
            if (isMem(a) && isReg(b)) {                     // MOV r/m32, r32   89 /r
                put8(out, 0x89);
                return emitModRM(out, code(b.reg), a, fixups, idx);
            }
            if (isMem(a) && isImm(b)) {                     // MOV r/m32, imm32 C7 /0
                put8(out, 0xC7);
                Status s = emitModRM(out, 0, a, fixups, idx);
                if (s != Status::OK) return s;
                put32(out, static_cast<uint32_t>(b.imm));
                return Status::OK;
            }
            return Status::BAD_OPERANDS;
        }

        // ===== added instruction set ===========================================
        case Op::HLT:  
        case Op::CLC:   
        case Op::STC:    
        case Op::CLD:
        case Op::STD:  
        case Op::CDQ:   
        case Op::LEAVE:  
        case Op::PUSHAD:
        case Op::POPAD: 
        case Op::PUSHFD: 
        case Op::POPFD:
            if (n != 0) return Status::BAD_OPERANDS;
            put8(out, singleByteOpcode(in.op));
            return Status::OK;

        case Op::NOT: 
        case Op::NEG: 
        case Op::MUL: 
        case Op::DIV: 
        case Op::IDIV: {
            if (n != 1 || !isRM(a)) return Status::BAD_OPERANDS;
            put8(out, 0xF7);                                // F7 /digit
            return emitModRM(out, unaryDigit(in.op), a, fixups, idx);
        }

        case Op::SHL: 
        case Op::SHR: 
        case Op::SAR: {
            if (n != 2 || !isRM(a) || !isImm(b)) return Status::BAD_OPERANDS;
            if (b.imm < 0 || b.imm > 31) return Status::BAD_IMMEDIATE;
            put8(out, 0xC1);                                // C1 /digit ib
            Status s = emitModRM(out, shiftDigit(in.op), a, fixups, idx);
            if (s != Status::OK) return s;
            put8(out, static_cast<uint32_t>(b.imm));
            return Status::OK;
        }

        case Op::INT:
            if (n != 1 || !isImm(a)) return Status::BAD_OPERANDS;
            if (a.imm < 0 || a.imm > 255) return Status::BAD_IMMEDIATE;
            put8(out, 0xCD);                                // CD ib
            put8(out, static_cast<uint32_t>(a.imm));
            return Status::OK;

        case Op::XCHG:
            if (n != 2) return Status::BAD_OPERANDS;
            if (isRM(a) && isReg(b)) {                      // XCHG r/m32, r32   87 /r
                put8(out, 0x87);
                return emitModRM(out, code(b.reg), a, fixups, idx);
            }
            if (isReg(a) && isMem(b)) {                     // XCHG r32, m32  (operands commute)
                put8(out, 0x87);
                return emitModRM(out, code(a.reg), b, fixups, idx);
            }
            return Status::BAD_OPERANDS;

        case Op::IMUL:                                      // IMUL r32, r/m32   0F AF /r
            if (n != 2 || !isReg(a) || !isRM(b)) return Status::BAD_OPERANDS;
            put8(out, 0x0F);
            put8(out, 0xAF);
            return emitModRM(out, code(a.reg), b, fixups, idx);

        case Op::TEST: {
            if (n != 2) return Status::BAD_OPERANDS;
            if (isRM(a) && isImm(b)) {
                if (isReg(a) && a.reg == Reg::EAX) {        // TEST EAX, imm32   A9 id  (5 bytes)
                    put8(out, 0xA9);
                    put32(out, static_cast<uint32_t>(b.imm));
                    return Status::OK;
                }
                put8(out, 0xF7);                            // TEST r/m32, imm32 F7 /0 id
                Status s = emitModRM(out, 0, a, fixups, idx);
                if (s != Status::OK) return s;
                put32(out, static_cast<uint32_t>(b.imm));
                return Status::OK;
            }
            if (isRM(a) && isReg(b)) {                      // TEST r/m32, r32   85 /r
                put8(out, 0x85);
                return emitModRM(out, code(b.reg), a, fixups, idx);
            }
            if (isReg(a) && isMem(b)) {                     // TEST r32, m32  (operands commute)
                put8(out, 0x85);
                return emitModRM(out, code(a.reg), b, fixups, idx);
            }
            return Status::BAD_OPERANDS;
        }

        case Op::JL: 
        case Op::JGE: 
        case Op::JLE: 
        case Op::JG:
        case Op::JB: 
        case Op::JAE: 
        case Op::JBE: 
        case Op::JA:
        case Op::JS: 
        case Op::JNS: {
            if (n != 1 || a.kind != OperandKind::LABEL) return Status::BAD_OPERANDS;
            put8(out, 0x0F);                                // Jcc rel32   0F 8x id
            put8(out, jccOpcode(in.op));
            Fixup f = { out.size(), a.label, idx, false };
            fixups.push_back(f);
            put32(out, 0);                                  // rel32 placeholder
            return Status::OK;
        }
        // =======================================================================

        default:
            break;                                          // ALU group below
    }

     uint8_t rmR, rRm, ext;
    if (!aluForm(in.op, rmR, rRm, ext)) return Status::BAD_OPCODE;
    if (n != 2) return Status::BAD_OPERANDS;

    if (isRM(a) && isImm(b)) {
        const bool short8 = fitsInt8(b.imm);
        // op EAX, imm32  (opcode+4, no ModRM) -- the 5-byte short form that
        // Worklet 1's Pass 1 assumes whenever the immediate doesn't fit in 8 bits.
        if (!short8 && isReg(a) && a.reg == Reg::EAX) {
            put8(out, rmR + 4u);
            put32(out, static_cast<uint32_t>(b.imm));
            return Status::OK;
        }
        put8(out, short8 ? 0x83u : 0x81u);                  // 83 /d ib  or  81 /d id
        Status s = emitModRM(out, ext, a, fixups, idx);
        if (s != Status::OK) return s;
        if (short8) put8(out, static_cast<uint32_t>(b.imm));
        else        put32(out, static_cast<uint32_t>(b.imm));
        return Status::OK;
    }
    if (isReg(a) && isRM(b)) {                              // op r32, r/m32   /r
        put8(out, rRm);
        return emitModRM(out, code(a.reg), b, fixups, idx);
    }
    if (isMem(a) && isReg(b)) {                             // op r/m32, r32   /r
        put8(out, rmR);
        return emitModRM(out, code(b.reg), a, fixups, idx);
    }
    return Status::BAD_OPERANDS;
}

Result fail(Status s, size_t idx) {
    Result r = { s, idx };
    return r;
}

Result encodeImpl(const Program& prog, std::vector<uint8_t>& out,
                  std::vector<size_t>* offs) {
    out.reserve(prog.count * 8);
    if (offs) offs->reserve(prog.count + 1);

    std::vector<size_t> labelOffset(prog.labelCount, kUndefined);   // O(1) lookup by label ID
    std::vector<Fixup>  fixups;

    // Single pass: emit bytes, record where each label lands.
    for (size_t i = 0; i < prog.count; ++i) {
        const Instruction& in = prog.instrs[i];

        if (in.defLabel != NO_LABEL) {
            if (in.defLabel >= prog.labelCount)          return fail(Status::LABEL_OUT_OF_RANGE, i);
            if (labelOffset[in.defLabel] != kUndefined)  return fail(Status::DUPLICATE_LABEL, i);
            labelOffset[in.defLabel] = out.size();
        }
        if (offs) offs->push_back(out.size());

        Status s = encodeOne(in, out, fixups, i);
        if (s != Status::OK) return fail(s, i);
    }
    if (offs) offs->push_back(out.size());                  // total size

    // A label that sits after the last instruction.
    if (prog.endLabel != NO_LABEL) {
        if (prog.endLabel >= prog.labelCount)            return fail(Status::LABEL_OUT_OF_RANGE, prog.count);
        if (labelOffset[prog.endLabel] != kUndefined)    return fail(Status::DUPLICATE_LABEL, prog.count);
        labelOffset[prog.endLabel] = out.size();
    }

    // Back-patch label references.
    //   relative: rel32 = target - (address of the byte after the rel32 field)
    //   absolute: baseAddress + target
    for (size_t k = 0; k < fixups.size(); ++k) {
        const Fixup& f = fixups[k];
        if (f.label >= prog.labelCount)        return fail(Status::LABEL_OUT_OF_RANGE, f.instr);
        const size_t target = labelOffset[f.label];
        if (target == kUndefined)              return fail(Status::UNDEFINED_LABEL, f.instr);

        uint32_t v;
        if (f.absolute) {
            v = prog.baseAddress + static_cast<uint32_t>(target);
        } else {
            const int64_t rel = static_cast<int64_t>(target) - static_cast<int64_t>(f.pos + 4);
            v = static_cast<uint32_t>(rel);                 // two's-complement wrap is intended
        }
        patch32(out, f.pos, v);
    }

    return fail(Status::OK, 0);
}

} // namespace

const char* statusText(Status s) {
    switch (s) {
        case Status::OK:                 return "OK";
        case Status::BAD_OPCODE:         return "opcode not handled";
        case Status::BAD_OPERANDS:       return "wrong operand count or unsupported operand combination";
        case Status::BAD_SCALE:          return "memory scale must be 1, 2, 4 or 8";
        case Status::BAD_INDEX_REG:      return "ESP cannot be an index register";
        case Status::LABEL_OUT_OF_RANGE: return "label ID out of range";
        case Status::DUPLICATE_LABEL:    return "label defined twice";
        case Status::UNDEFINED_LABEL:    return "label referenced but never defined";
        case Status::BAD_IMMEDIATE:      return "immediate out of range for this instruction";
    }
    return "unknown status";
}

Result encode(const Program& prog, std::vector<uint8_t>& out, std::vector<size_t>* instrOffsets) {
    out.clear();
    if (instrOffsets) instrOffsets->clear();

    Result r = encodeImpl(prog, out, instrOffsets);
    if (r.status != Status::OK) {
        out.clear();
        if (instrOffsets) instrOffsets->clear();
    }
    return r;
}

bool writeBinFile(const char* path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(&bytes[0]), static_cast<std::streamsize>(bytes.size()));
    }
    f.close();
    return !f.fail();
}

} 