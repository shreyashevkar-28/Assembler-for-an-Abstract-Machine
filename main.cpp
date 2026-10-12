#define main worklet1_main
#include "worklet1.cpp"
#undef main

#include "worklet2.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

// ---------------------------------------------------------------------------
// Adapter: Worklet 1 IR (strings) -> Worklet 2 input (enums + integers)
// ---------------------------------------------------------------------------
namespace {

struct LowerError {
    string message;
    int    line;     // 0 if not tied to a source line
};

bool lookupOp(const string& m, w2::Op& op) {
    if (m == "MOV")  { op = w2::Op::MOV;  return true; }
    if (m == "ADD")  { op = w2::Op::ADD;  return true; }
    if (m == "SUB")  { op = w2::Op::SUB;  return true; }
    if (m == "CMP")  { op = w2::Op::CMP;  return true; }
    if (m == "INC")  { op = w2::Op::INC;  return true; }
    if (m == "DEC")  { op = w2::Op::DEC;  return true; }
    if (m == "PUSH") { op = w2::Op::PUSH; return true; }
    if (m == "POP")  { op = w2::Op::POP;  return true; }
    if (m == "JMP")  { op = w2::Op::JMP;  return true; }
    if (m == "CALL") { op = w2::Op::CALL; return true; }
    if (m == "JE")   { op = w2::Op::JE;   return true; }
    if (m == "JNE")  { op = w2::Op::JNE;  return true; }
    if (m == "NOP")  { op = w2::Op::NOP;  return true; }
    if (m == "RET")  { op = w2::Op::RET;  return true; }
}