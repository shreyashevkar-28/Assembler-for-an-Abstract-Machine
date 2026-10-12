#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace w2 {

// ---- Strongly typed vocabulary ------------------------------------------

// Numbered exactly as the hardware encodes them in ModRM/SIB fields.
enum class Reg : uint8_t {
    EAX = 0, ECX = 1, EDX = 2, EBX = 3, ESP = 4, EBP = 5, ESI = 6, EDI = 7,
    NONE = 0xFF
};

enum class Op : uint8_t {
    MOV, ADD, SUB, AND, OR, XOR, CMP,   // two operands
    PUSH, POP, INC, DEC,                // one register operand
    JMP, CALL, JE, JNE,                 // one LABEL operand (rel32, near form)
    RET, NOP,                           // no operands

    // ---- added instruction set -------------------------------------------
    TEST, XCHG, IMUL,                   // two operands (IMUL = r32, r/m32 form)
    SHL, SHR, SAR,                      // r/m32, imm8 (count 0..31)
    NOT, NEG, MUL, DIV, IDIV,           // one register operand (F7 group)
    INT,                                // one imm8 operand
    JL, JGE, JLE, JG,                   // signed conditional jumps  (rel32, near)
    JB, JAE, JBE, JA,                   // unsigned conditional jumps (rel32, near)
    JS, JNS,                            // sign-flag conditional jumps (rel32, near)
    HLT, CLC, STC, CLD, STD, CDQ,       // no operands, single byte
    LEAVE, PUSHAD, POPAD, PUSHFD, POPFD // no operands, single byte
};

// MEMLABEL = absolute memory operand "[label]"; encoded as [disp32] where
// disp32 = Program::baseAddress + offset of the label.
enum class OperandKind : uint8_t { NONE, REG, IMM, MEM, LABEL, MEMLABEL };

typedef uint32_t LabelId;
constexpr LabelId NO_LABEL = 0xFFFFFFFFu;

// [base + index*scale + disp].  Absent parts are Reg::NONE; with neither
// base nor index, `disp` is an absolute address.
struct MemOperand {
    Reg     base;
    Reg     index;   // ESP is not encodable as an index
    uint8_t scale;   // 1, 2, 4 or 8 (ignored when index == NONE)
    int32_t disp;
};

struct Operand {
    OperandKind kind;
    union {
        Reg        reg;     // kind == REG
        int32_t    imm;     // kind == IMM
        LabelId    label;   // kind == LABEL or MEMLABEL
        MemOperand mem;     // kind == MEM
    };
};

struct Instruction {
    Op       op;
    uint8_t  operandCount;   // 0..2
    Operand  operands[2];    // operands[0] is the destination (Intel order)
    LabelId  defLabel;       // label defined at this instruction, or NO_LABEL
};

static_assert(std::is_pod<Instruction>::value, "Instruction must stay plain-old-data");

struct Program {
    const Instruction* instrs;
    size_t             count;
    uint32_t           labelCount;    // valid label IDs are [0, labelCount)
    uint32_t           baseAddress;   // added to label offsets for absolute ([label]) operands
    LabelId            endLabel;      // label located after the last instruction, or NO_LABEL
};

static_assert(std::is_pod<Program>::value, "Program must stay plain-old-data");

// ---- Convenience builders (pure struct filling) ----------------------------

inline Operand regOp(Reg r) {
    Operand o = Operand(); o.kind = OperandKind::REG; o.reg = r; return o;
}
inline Operand immOp(int32_t v) {
    Operand o = Operand(); o.kind = OperandKind::IMM; o.imm = v; return o;
}
inline Operand labelOp(LabelId id) {
    Operand o = Operand(); o.kind = OperandKind::LABEL; o.label = id; return o;
}
inline Operand labelMemOp(LabelId id) {
    Operand o = Operand(); o.kind = OperandKind::MEMLABEL; o.label = id; return o;
}
inline Operand memOp(Reg base, Reg index, uint8_t scale, int32_t disp) {
    Operand o = Operand(); o.kind = OperandKind::MEM;
    o.mem.base = base; o.mem.index = index; o.mem.scale = scale; o.mem.disp = disp;
    return o;
}

inline Instruction inst0(Op op, LabelId def = NO_LABEL) {
    Instruction i = Instruction(); i.op = op; i.operandCount = 0; i.defLabel = def; return i;
}
inline Instruction inst1(Op op, Operand a, LabelId def = NO_LABEL) {
    Instruction i = Instruction(); i.op = op; i.operandCount = 1; i.operands[0] = a; i.defLabel = def; return i;
}
inline Instruction inst2(Op op, Operand a, Operand b, LabelId def = NO_LABEL) {
    Instruction i = Instruction(); i.op = op; i.operandCount = 2;
    i.operands[0] = a; i.operands[1] = b; i.defLabel = def; return i;
}

inline Program makeProgram(const Instruction* instrs, size_t count, uint32_t labelCount,
                           uint32_t baseAddress = 0, LabelId endLabel = NO_LABEL) {
    Program p = Program();
    p.instrs = instrs; p.count = count; p.labelCount = labelCount;
    p.baseAddress = baseAddress; p.endLabel = endLabel;
    return p;
}

// ---- Encoder API ----------------------------------------------------------

enum class Status : uint8_t {
    OK,
    BAD_OPCODE,          // op value not handled
    BAD_OPERANDS,        // wrong count or unsupported operand combination
    BAD_SCALE,           // memory scale not 1/2/4/8
    BAD_INDEX_REG,       // ESP used as index
    LABEL_OUT_OF_RANGE,  // label ID >= labelCount
    DUPLICATE_LABEL,     // same label ID defined twice
    UNDEFINED_LABEL,     // label referenced but never defined
    BAD_IMMEDIATE        // immediate out of range for this instruction (shift count, INT number)
};

struct Result {
    Status status;
    size_t instrIndex;   // index of the offending instruction (valid when status != OK);
                         // == prog.count when the problem is Program::endLabel
};

// Short human-readable name of a status code.
const char* statusText(Status s);

// Encodes `prog` into raw x86-32 machine code (`out` is cleared first).
// Single pass: each instruction is emitted as it is visited, and label
// references are back-patched once every label offset is known.
//
// If `instrOffsets` is non-null it receives prog.count + 1 entries: the byte
// offset at which each instruction starts, plus the total size as the last
// entry.  Worklet 1's Pass 1 already assigned an address and length to every
// instruction; comparing against these offsets proves both sides agree.
//
// On failure `out` (and `instrOffsets`) are left empty.
Result encode(const Program& prog, std::vector<uint8_t>& out,
              std::vector<size_t>* instrOffsets = nullptr);

// Writes `bytes` verbatim to `path` (binary mode). Returns false on I/O error.
bool writeBinFile(const char* path, const std::vector<uint8_t>& bytes);

} // namespace w2
