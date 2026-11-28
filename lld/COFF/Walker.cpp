//===- Walker.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Walker.h"
#include "Chunks.h"
#include "Driver.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Config/Targets.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCDisassembler/MCSymbolizer.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Win64EH.h"
#include <map>
#include <set>
#if LLVM_HAS_X86_TARGET
// The operands' types, before the register classes whose names they share.
#define GET_INSTRINFO_OPERAND_TYPES_ENUM
#define GET_INSTRINFO_OPERAND_TYPE
#define GET_INSTRINFO_MEM_OPERAND_SIZE
#include "X86GenInstrInfo.inc"

#include "MCTargetDesc/X86BaseInfo.h"
#endif

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::codeview;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::coff;

static const char unreached[] = "no proven path reaches it";
static const char offOperand[] =
    "a relocation is not on an operand of the instruction that holds it";
static const char overlap[] = "two instructions, or one and data, overlap";
static const char unexplained[] =
    "bytes that are no instruction, padding or marked data";
static const char unknown[] = "an unknown or divergent encoding";
static const char nonCanonical[] = "a form of the instruction that is not "
                                   "canonical";

//===----------------------------------------------------------------------===//
// The decoder.
//===----------------------------------------------------------------------===//

namespace {
// Records the fields of the instruction that the disassembler is decoding,
// which reports each immediate and each memory operand's displacement.
class Recorder : public MCSymbolizer {
public:
  Recorder(MCContext &ctx, MCRegister rip, MCRegister eip)
      : MCSymbolizer(ctx, nullptr), rip(rip), eip(eip) {}

  bool tryAddingSymbolicOperand(MCInst &inst, raw_ostream &, int64_t, uint64_t,
                                bool isBranch, uint64_t offset, uint64_t size,
                                uint64_t) override {
    // A displacement follows its operand's base, scale and index. Only a base
    // is ever the instruction pointer.
    unsigned n = inst.getNumOperands();
    MCRegister base;
    if (!isBranch && n >= 3 && inst.getOperand(n - 3).isReg())
      base = inst.getOperand(n - 3).getReg();
    if (!size)
      return false;
    // The disassembler keeps one offset for an instruction's immediates.
    for (const InsnField &f : fields)
      ambiguous |=
          offset < unsigned(f.offset + f.size) && f.offset < offset + size;
    fields.push_back({uint8_t(offset), uint8_t(size), isBranch,
                      base && (base == rip || base == eip)});
    return false;
  }
  void tryAddingPcLoadReferenceComment(raw_ostream &, int64_t,
                                       uint64_t) override {}

  SmallVector<InsnField, 4> fields;
  bool ambiguous = false;

private:
  MCRegister rip, eip;
};
} // namespace

struct X86Decoder::Impl {
  std::unique_ptr<MCRegisterInfo> mri;
  std::unique_ptr<MCAsmInfo> mai;
  std::unique_ptr<MCSubtargetInfo> sti;
  std::unique_ptr<MCInstrInfo> mii;
  std::unique_ptr<MCContext> ctx;
  std::unique_ptr<MCDisassembler> dis;
  std::unique_ptr<MCInstrAnalysis> mia;
  std::unique_ptr<MCCodeEmitter> emitter;
  Recorder *recorder;
  MCRegister cs;
  const MCExpr *zero;
  // The instruction that decode read last.
  MCInst inst;
};

X86Decoder::X86Decoder(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
X86Decoder::~X86Decoder() = default;

std::unique_ptr<X86Decoder> X86Decoder::create() {
#if LLVM_HAS_X86_TARGET
  static std::once_flag once;
  std::call_once(once, [] {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86Disassembler();
  });
  Triple triple("x86_64-pc-windows-msvc");
  std::string error;
  const Target *target = TargetRegistry::lookupTarget(triple, error);
  if (!target)
    return nullptr;
  auto m = std::make_unique<Impl>();
  m->mri.reset(target->createMCRegInfo(triple));
  m->mai.reset(target->createMCAsmInfo(*m->mri, triple, MCTargetOptions()));
  m->sti.reset(target->createMCSubtargetInfo(triple, "", ""));
  m->mii.reset(target->createMCInstrInfo());
  m->ctx = std::make_unique<MCContext>(triple, *m->mai, *m->mri, *m->sti);
  m->dis.reset(target->createMCDisassembler(*m->sti, *m->ctx));
  m->mia.reset(target->createMCInstrAnalysis(m->mii.get()));
  m->emitter.reset(target->createMCCodeEmitter(*m->mii, *m->ctx));
  m->zero = MCConstantExpr::create(0, *m->ctx);
  MCRegister eip;
  for (unsigned r = 1, e = m->mri->getNumRegs(); r != e; ++r) {
    if (StringRef(m->mri->getName(r)) == "EIP")
      eip = r;
    if (StringRef(m->mri->getName(r)) == "CS")
      m->cs = r;
  }
  auto recorder =
      std::make_unique<Recorder>(*m->ctx, m->mri->getProgramCounter(), eip);
  m->recorder = recorder.get();
  m->dis->setSymbolizer(std::move(recorder));
  return std::unique_ptr<X86Decoder>(new X86Decoder(std::move(m)));
#else
  return nullptr;
#endif
}

static bool isLegacyPrefix(uint8_t b) {
  return StringRef("\x26\x2e\x36\x3e\x64\x65\x66\x67\xf0\xf2\xf3")
      .contains(char(b));
}

// Returns whether a and b are one instruction. Two encodings of one
// instruction decode alike, so this is the test of a round trip, not the
// bytes. A prefix that the instruction ignores is not encoded again.
static bool isSame(const MCInst &a, const MCInst &b) {
  if (a.getOpcode() != b.getOpcode() ||
      a.getNumOperands() != b.getNumOperands())
    return false;
  for (unsigned i = 0, e = a.getNumOperands(); i != e; ++i) {
    const MCOperand &x = a.getOperand(i), &y = b.getOperand(i);
    if (x.isReg() ? !y.isReg() || x.getReg() != y.getReg()
                  : !x.isImm() || !y.isImm() || x.getImm() != y.getImm())
      return false;
  }
  return true;
}

// Returns whether the instruction at code, after its prefixes, takes lock:
// one that reads and writes its destination in memory. The instruction
// tables do not say which those are.
static bool isLockable(ArrayRef<uint8_t> code) {
  bool wide = !code.empty() && code[0] == 0x0f;
  if (code.size() < 2u + wide || (code[1 + wide] >> 6) == 3)
    return false;
  int op = code[wide], reg = (code[1 + wide] >> 3) & 7;
  if (wide)
    // bts, btr, btc, cmpxchg, xadd, and cmpxchg8b or 16b.
    return is_contained({0xab, 0xb3, 0xbb, 0xb0, 0xb1, 0xc0, 0xc1}, op) ||
           (op == 0xba && reg >= 5) || (op == 0xc7 && reg == 1);
  // add, or, adc, sbb, and, sub and xor to memory, from a register or an
  // immediate; xchg; not and neg; inc and dec.
  return (op < 0x38 && !(op & 0xc6)) ||
         (is_contained({0x80, 0x81, 0x83}, op) && reg != 7) ||
         (op & 0xfe) == 0x86 || ((op & 0xfe) == 0xf6 && (reg & 6) == 2) ||
         ((op & 0xfe) == 0xfe && reg < 2);
}

// Returns the offset of the opcode of the direct branch whose offset is f.
static unsigned getBranchOpcode(ArrayRef<uint8_t> code, const InsnField &f) {
  return f.offset - (f.offset >= 2 && code[f.offset - 2] == 0x0f ? 2 : 1);
}

bool X86Decoder::decode(ArrayRef<uint8_t> code, uint64_t address, Insn &insn) {
  Impl &m = *impl;
  MCInst &inst = m.inst, again;
  uint64_t size, sizeAgain;
  // The disassembler takes encodings that no processor does. A VEX, EVEX or
  // XOP prefix after 66, lock or rep, or right after REX.
  ArrayRef<uint8_t> prefixes = code.take_while(
      [](uint8_t b) { return isLegacyPrefix(b) || (b & 0xf0) == 0x40; });
  ArrayRef<uint8_t> rest = code.drop_front(prefixes.size());
  if (!rest.empty() &&
      (is_contained({0xc4, 0xc5, 0x62}, rest[0]) ||
       (rest[0] == 0x8f && rest.size() > 1 && (rest[1] & 0x1f) >= 8)) &&
      !prefixes.empty() &&
      ((prefixes.back() & 0xf0) == 0x40 ||
       llvm::any_of(prefixes,
                    [](uint8_t b) { return b == 0x66 || b >= 0xf0; })))
    return false;
  // lock on an instruction that takes none.
  if (is_contained(prefixes, 0xf0) && !isLockable(rest))
    return false;
  // REX2 before an opcode that it is reserved with: rows 4, 7, A and E of the
  // first map, 3 and 8 of the second.
  if (rest.size() > 2 && rest[0] == 0xd5) {
    uint8_t row = rest[2] >> 4;
    if (rest[1] & 0x80 ? row == 3 || row == 8
                       : is_contained({4, 7, 0xa, 0xe}, row))
      return false;
  }
  m.recorder->fields.clear();
  m.recorder->ambiguous = false;
  if (m.dis->getInstruction(inst, size, code, address, nulls()) !=
      MCDisassembler::Success)
    return false;
  // The disassembler gives lock, and rep before some opcodes, as an
  // instruction of its own. It is part of the instruction after it, as is
  // no other prefix that the disassembler leaves alone.
  if (size == 1 && (isLegacyPrefix(code[0]) || (code[0] & 0xf0) == 0x40)) {
    if (code[0] < 0xf0 || !decode(code.drop_front(), address + 1, insn) ||
        insn.size == 15)
      return false;
    ++insn.size;
    for (InsnField &f : insn.fields)
      ++f.offset;
    return true;
  }
  insn.size = size;
  insn.fields.clear();
  if (!m.recorder->ambiguous)
    insn.fields = m.recorder->fields;
  // A load of cs.
  if (m.mii->get(inst.getOpcode()).getNumDefs() && inst.getOperand(0).isReg() &&
      inst.getOperand(0).getReg() == m.cs)
    return false;
  // rep before an opcode of the second map that is the same instruction
  // without it. A jcc takes it as bnd.
  if (rest.size() > 1 && rest[0] == 0x0f && (rest[1] & 0xf0) != 0x80 &&
      (is_contained(prefixes, 0xf2) || is_contained(prefixes, 0xf3))) {
    SmallVector<uint8_t, 15> bare;
    llvm::copy_if(prefixes, std::back_inserter(bare),
                  [](uint8_t b) { return b != 0xf2 && b != 0xf3; });
    bare.append(rest.begin(), rest.end());
    if (m.dis->getInstruction(again, sizeAgain, bare, address, nulls()) ==
            MCDisassembler::Success &&
        again.getOpcode() == inst.getOpcode())
      return false;
  }

  // The instruction must be one that the encoder writes and the disassembler
  // then reads back.
  SmallString<16> bytes;
  SmallVector<MCFixup, 2> fixups;
  m.emitter->encodeInstruction(inst, bytes, fixups, *m.sti);
  // The encoder leaves a branch's offset, the last field, to a fixup.
  for (const MCFixup &fixup : fixups) {
    int64_t value;
    if (!fixup.getValue()->evaluateAsAbsolute(value))
      return false;
    for (size_t i = fixup.getOffset(); i < bytes.size(); ++i, value >>= 8)
      bytes[i] = value;
  }
  // The encoder writes lock and rep first, where the disassembler gives them
  // alone.
  for (ArrayRef<uint8_t> written = arrayRefFromStringRef(bytes);;
       written = written.drop_front()) {
    if (written.empty() ||
        m.dis->getInstruction(again, sizeAgain, written, address, nulls()) !=
            MCDisassembler::Success)
      return false;
    if (sizeAgain != 1 || !isLegacyPrefix(written[0]))
      break;
  }
  if (!isSame(inst, again))
    return false;

  // Before a branch, 66 keeps the offset at 32 bits on Intel processors and
  // makes it 16 on AMD's.
  for (const InsnField &f : insn.fields)
    if (f.branch &&
        is_contained(code.take_front(getBranchOpcode(code, f)), 0x66))
      return false;
  uint64_t target;
  insn.target.reset();
  if (m.mia->evaluateBranch(inst, address, size, target) &&
      (m.mia->isBranch(inst) || m.mia->isCall(inst)))
    insn.target = target;
  // A return, a jump, or a trap: int3, hlt, ud2 and int 29h.
  insn.ends = m.mia->isReturn(inst) || m.mia->isUnconditionalBranch(inst) ||
              m.mia->isIndirectBranch(inst) ||
              (size == 1 && (code[0] == 0xcc || code[0] == 0xf4)) ||
              (size == 2 && ((code[0] == 0x0f && code[1] == 0x0b) ||
                             (code[0] == 0xcd && code[1] == 0x29)));
  return true;
}

bool X86Decoder::decodeRead(ArrayRef<uint8_t> code, ReadInsn &insn) {
#if LLVM_HAS_X86_TARGET
  Insn decoded;
  if (!decode(code, 0, decoded))
    return false;
  Impl &m = *impl;
  const MCInst &inst = m.inst;
  const MCInstrDesc &desc = m.mii->get(inst.getOpcode());
  insn.size = decoded.size;
  insn.reads = desc.mayLoad() && !desc.isBranch() && !desc.isCall();
  insn.stack = desc.hasImplicitUseOfPhysReg(X86::RSP) ||
               desc.hasImplicitDefOfPhysReg(X86::RSP, m.mri.get()) ||
               llvm::any_of(inst, [&](const MCOperand &op) {
                 return op.isReg() && op.getReg() &&
                        m.mri->isSuperOrSubRegisterEq(X86::RSP, op.getReg());
               });
  bool lea = inst.getOpcode() == X86::LEA64r;
  insn.address = lea;
  insn.value = (lea || inst.getOpcode() == X86::MOV64rm) &&
                       inst.getOperand(1 + X86::AddrBaseReg).getReg() ==
                           X86::RIP &&
                       !inst.getOperand(1 + X86::AddrSegmentReg).getReg()
                   ? m.mri->getEncodingValue(inst.getOperand(0).getReg())
                   : -1;
  // The width of the memory operand, in the target's tables; an operand of
  // no stated width has none.
  int mem = X86II::getMemoryOperandNo(desc.TSFlags);
  insn.width = mem < 0
                   ? 0
                   : X86::getMemOperandSize(X86::getOperandType(
                         inst.getOpcode(), mem + X86II::getOperandBias(desc))) /
                         8;
  return true;
#else
  return false;
#endif
}

bool X86Decoder::decodeFused(ArrayRef<uint8_t> code, FusedInsn &insn,
                             SmallVectorImpl<char> *direct, unsigned *field,
                             bool second) {
#if LLVM_HAS_X86_TARGET
  Impl &m = *impl;
  Insn decoded;
  unsigned regOp;
  if (!decode(code, 0, decoded))
    return false;
  MCInst &inst = m.inst;
  const MCInstrDesc &desc = m.mii->get(inst.getOpcode());
  insn.use = X86::getFusedUse(inst, desc, regOp);
  if (insn.use == FusedUse::None)
    return false;
  unsigned mem = regOp;
  if (insn.use == FusedUse::Index && second)
    regOp = getFusedIndexOperand(inst, regOp);
  MCRegister reg = inst.getOperand(regOp).getReg();
  if (reg == m.mri->getProgramCounter() ||
      !MCWinCOFFStreamer::readsFusedRegisterOnce(inst, desc, *m.mri, insn.use,
                                                 regOp))
    return false;
  insn.reg = m.mri->getEncodingValue(reg);
  insn.size = decoded.size;
  insn.disp = 0;
  bool wide = inst.getOpcode() == X86::MOV64rm;
  insn.loads = wide ? 8 : inst.getOpcode() == X86::MOV32rm ? 4 : 0;
  insn.dest = insn.loads || inst.getOpcode() == X86::MOVSX64rm32
                  ? m.mri->getEncodingValue(inst.getOperand(0).getReg())
                  : 0;
  if (insn.use != FusedUse::Add && insn.use != FusedUse::Access &&
      insn.use != FusedUse::Index)
    return true;
  MCOperand &disp = inst.getOperand(mem + X86::AddrDisp);
  insn.disp = disp.getImm();
  if (!direct)
    return true;
  // The disassembler gives lock as an instruction of its own, so the encoder
  // writes none; an instruction it writes shorter for another reason has no
  // direct form here. It leaves a displacement that is an expression to a
  // fixup, which says where the displacement is.
  SmallString<16> same;
  SmallVector<MCFixup, 1> fixups;
  m.emitter->encodeInstruction(inst, same, fixups, *m.sti);
  bool lock = same.size() + 1 == insn.size && code[0] == 0xf0;
  if (same.size() + lock != insn.size)
    return true;
  if (lock)
    direct->push_back(char(0xf0));
  // The register that an index keeps is the base of its direct form.
  if (insn.use == FusedUse::Index) {
    MCOperand &index = inst.getOperand(mem + X86::AddrIndexReg);
    if (!second)
      inst.getOperand(mem).setReg(index.getReg());
    index.setReg(MCRegister());
  } else {
    inst.getOperand(regOp).setReg(m.mri->getProgramCounter());
  }
  disp = MCOperand::createExpr(m.zero);
  m.emitter->encodeInstruction(inst, *direct, fixups, *m.sti);
  *field = fixups.back().getOffset() + lock;
  return true;
#else
  return false;
#endif
}

bool X86Decoder::encodeOnRegisterOf(ArrayRef<uint8_t> code, bool second,
                                    ArrayRef<uint8_t> from, bool fromSecond,
                                    SmallVectorImpl<char> &out) {
#if LLVM_HAS_X86_TARGET
  Impl &m = *impl;
  Insn decoded;
  unsigned regOp;
  // The operand of an index that the register it reads is added to.
  auto other = [&](bool second) -> MCOperand * {
    MCInst &inst = m.inst;
    if (X86::getFusedUse(inst, m.mii->get(inst.getOpcode()), regOp) !=
        FusedUse::Index)
      return nullptr;
    return &inst.getOperand(second ? regOp
                                   : COFF::getFusedIndexOperand(inst, regOp));
  };
  MCOperand *op;
  if (!decode(from, 0, decoded) || !(op = other(fromSecond)))
    return false;
  MCRegister reg = op->getReg();
  if (!decode(code, 0, decoded) || !(op = other(second)))
    return false;
  op->setReg(reg);
  SmallVector<MCFixup, 1> fixups;
  m.emitter->encodeInstruction(m.inst, out, fixups, *m.sti);
  return fixups.empty();
#else
  return false;
#endif
}

X86Decoder *X86Decoder::get() {
  static thread_local std::unique_ptr<X86Decoder> decoder = create();
  return decoder.get();
}

//===----------------------------------------------------------------------===//
// What an object states about its code.
//===----------------------------------------------------------------------===//

namespace {
struct Extent {
  uint32_t begin, end;
  bool contains(uint32_t off) const { return begin <= off && off < end; }
};

// A CodeView procedure, whose frame may state that it has no inline assembly.
struct Proc : Extent {
  bool noInlineAsm;
};

// A place in the object: a section's number and an offset in it.
using Place = std::pair<uint32_t, uint32_t>;

// A REL32 relocation that names a place in code.
struct RelativeRel {
  Place at, to;
};

struct CodeSection {
  // The extents of functions: .pdata's, CodeView's procedures, and the
  // function-typed symbols with their indices.
  std::vector<Extent> unwound;
  std::vector<Proc> procs;
  std::vector<std::pair<uint32_t, uint32_t>> funcs;
  // Proven instruction starts: of functions and procedures, of unwind ranges
  // and what chains to them, after each instruction of a prologue, and where
  // an exception handler resumes or an exception lands.
  std::vector<uint32_t> anchors;
  // Places that words of data name, which may be instruction starts.
  std::vector<uint32_t> weak;
  // Places that no instruction may straddle: every symbol, label and line,
  // the ends of unwind ranges and scopes, and the bounds of call sites.
  std::vector<uint32_t> checks;
  // Where data begins: the static symbols, and CodeView's data.
  std::vector<uint32_t> statics, data;
  // The places that REL32 relocations of other code sections name, each with
  // the relocation's own place, and the starts of the functions decoded.
  std::vector<std::pair<uint32_t, Place>> named;
  DenseSet<uint32_t> walked;
  DenseMap<uint32_t, Site> sites;
};

struct RawSection {
  ArrayRef<uint8_t> data;
  // In the order of their offsets.
  std::vector<const coff_relocation *> rels;
  bool read = false;

  // The relocations that begin at or after off.
  auto from(uint32_t off) const {
    return partition_point(rels, [=](const coff_relocation *r) {
      return r->VirtualAddress < off;
    });
  }
};

// The bytes a relocation of the type fills; none for one that is not known.
unsigned getWidth(uint16_t type) {
  switch (type) {
  case IMAGE_REL_AMD64_ADDR64:
    return 8;
  case IMAGE_REL_AMD64_ADDR32:
  case IMAGE_REL_AMD64_ADDR32NB:
  case IMAGE_REL_AMD64_REL32:
  case IMAGE_REL_AMD64_REL32_1:
  case IMAGE_REL_AMD64_REL32_2:
  case IMAGE_REL_AMD64_REL32_3:
  case IMAGE_REL_AMD64_REL32_4:
  case IMAGE_REL_AMD64_REL32_5:
  case IMAGE_REL_AMD64_SECREL:
    return 4;
  case IMAGE_REL_AMD64_SECTION:
    return 2;
  case IMAGE_REL_AMD64_SECREL7:
    return 1;
  default:
    return 0;
  }
}

bool isRel32(uint16_t type) {
  return type >= IMAGE_REL_AMD64_REL32 && type <= IMAGE_REL_AMD64_REL32_5;
}
} // namespace

struct Walker::Object {
  Object(ObjFile &file);

  ObjFile &file;
  const COFFObjectFile &obj;
  std::vector<RawSection> sections;
  std::map<uint32_t, CodeSection> code;
  // The REL32 relocations that name code, by their own place.
  std::vector<RelativeRel> relative;
  // A compiler by @comp.id, whose output is instructions from each function's
  // start on; MSVC or MASM, whose static symbols in code are data; a compiler
  // by CodeView, which then states the functions without inline assembly.
  bool compiler = false, marksData = false, described = false;
  // The sites that the object lists as those that take a function's address,
  // as cl /Gu does, and the symbols whose address a table of it lists as
  // taken: its address-significance table, or that for Control Flow Guard of
  // an object that states it is guarded.
  std::optional<DenseSet<Place>> unique;
  std::optional<SmallPtrSet<Symbol *, 8>> taken;

  RawSection &section(uint32_t n);
  CodeSection *getCode(uint32_t n);
  std::optional<Place> getPlace(const coff_relocation &rel, uint64_t addend,
                                StringRef *name = nullptr);
  std::optional<Place> getPlace(uint32_t n, uint32_t off,
                                StringRef *name = nullptr);
  void readUnwind(Place info, uint32_t begin, CodeSection &cs);
  void readDebug(uint32_t n, ArrayRef<uint8_t> d);
  std::optional<Extent> getExtent(uint32_t n, uint32_t off, Site &site);
  bool walk(uint32_t n, Extent x, Site site);
  Site classify(uint32_t n, const coff_relocation &rel);
};

RawSection &Walker::Object::section(uint32_t n) {
  RawSection &s = sections[n];
  if (s.read)
    return s;
  s.read = true;
  const coff_section *sec = check(obj.getSection(n));
  if (Error e = obj.getSectionContents(sec, s.data))
    consumeError(std::move(e));
  for (const coff_relocation &rel : obj.getRelocations(sec))
    s.rels.push_back(&rel);
  llvm::stable_sort(s.rels, [](auto *a, auto *b) {
    return a->VirtualAddress < b->VirtualAddress;
  });
  return s;
}

CodeSection *Walker::Object::getCode(uint32_t n) {
  if (!n || n >= sections.size() ||
      !(check(obj.getSection(n))->Characteristics & IMAGE_SCN_CNT_CODE))
    return nullptr;
  return &code[n];
}

// Returns the place that rel names with the addend its field holds, if its
// symbol is defined in the object.
std::optional<Place> Walker::Object::getPlace(const coff_relocation &rel,
                                              uint64_t addend,
                                              StringRef *name) {
  if (rel.SymbolTableIndex >= obj.getNumberOfSymbols())
    return std::nullopt;
  COFFSymbolRef sym = check(obj.getSymbol(rel.SymbolTableIndex));
  if (name)
    *name = check(obj.getSymbolName(sym));
  int32_t n = sym.getSectionNumber();
  if (n <= 0 || uint32_t(n) >= sections.size())
    return std::nullopt;
  return Place(n, sym.getValue() + addend);
}

// Returns the place that the 32-bit field at off of section n names.
std::optional<Place> Walker::Object::getPlace(uint32_t n, uint32_t off,
                                              StringRef *name) {
  RawSection &s = section(n);
  auto it = s.from(off);
  if (it == s.rels.end() || (*it)->VirtualAddress != off ||
      uint64_t(off) + 4 > s.data.size())
    return std::nullopt;
  return getPlace(**it, read32le(&s.data[off]), name);
}

// Reads the unwind information at info of the range that begins at begin.
void Walker::Object::readUnwind(Place info, uint32_t begin, CodeSection &cs) {
  using namespace llvm::Win64EH;
  ArrayRef<uint8_t> d = section(info.first).data;
  if (uint64_t(info.second) + 4 > d.size())
    return;
  auto *ui = reinterpret_cast<const UnwindInfo *>(&d[info.second]);
  uint32_t more = (const uint8_t *)ui->getLanguageSpecificData() - d.data();
  // Later versions lay the codes out differently.
  if (ui->getVersion() > 2 || more > d.size())
    return;
  cs.anchors.push_back(begin + ui->PrologSize);
  for (unsigned i = 0; i < ui->NumCodes;) {
    const UnwindCode &c = ui->UnwindCodes[i];
    unsigned op = c.getUnwindOp();
    // An epilogue's code holds no offset in the prologue.
    if (op != UOP_Epilog)
      cs.anchors.push_back(begin + c.u.CodeOffset);
    i += op == UOP_AllocLarge ? (c.getOpInfo() ? 3 : 2)
         : op == UOP_SaveNonVolBig || op == UOP_SaveXMM128Big ? 3
         : op == UOP_SaveNonVol || op == UOP_SaveXMM128 || op == UOP_SpareCode
             ? 2
             : 1;
  }
  if (ui->getFlags() & UNW_ChainInfo) {
    if (std::optional<Place> parent = getPlace(info.first, more))
      if (CodeSection *to = getCode(parent->first))
        to->anchors.push_back(parent->second);
    return;
  }
  StringRef handler;
  if (!(ui->getFlags() & (UNW_ExceptionHandler | UNW_TerminateHandler)) ||
      uint64_t(more) + 8 > d.size())
    return;
  getPlace(info.first, more, &handler);
  // An Itanium personality's data is a table of call sites: the start and the
  // length of each, the place where an exception in it lands, and an action,
  // as offsets from the range's start. Only a table that is read whole, in an
  // encoding that is known, is taken.
  if (handler.ends_with("_personality_seh0")) {
    DataExtractor lsda(d.drop_front(more + 4), /*IsLittleEndian=*/true);
    DataExtractor::Cursor c(0);
    uint8_t landingBase = lsda.getU8(c);
    if (lsda.getU8(c) != dwarf::DW_EH_PE_omit)
      lsda.getULEB128(c);
    uint8_t encoding = lsda.getU8(c);
    uint64_t end = lsda.getULEB128(c);
    end += c.tell();
    bool known = landingBase == dwarf::DW_EH_PE_omit &&
                 (encoding == dwarf::DW_EH_PE_uleb128 ||
                  encoding == dwarf::DW_EH_PE_udata4);
    auto read = [&] {
      return encoding == dwarf::DW_EH_PE_uleb128 ? lsda.getULEB128(c)
                                                 : lsda.getU32(c);
    };
    std::vector<uint32_t> pads, sites;
    while (known && c && c.tell() < end) {
      uint64_t start = read(), length = read(), pad = read();
      lsda.getULEB128(c);
      sites.insert(sites.end(),
                   {uint32_t(begin + start), uint32_t(begin + start + length)});
      if (pad)
        pads.push_back(begin + pad);
    }
    if (known && c && c.tell() == end) {
      cs.anchors.insert(cs.anchors.end(), pads.begin(), pads.end());
      cs.checks.insert(cs.checks.end(), sites.begin(), sites.end());
    }
    consumeError(c.takeError());
    return;
  }
  // The C handler's scopes each end where the next instruction begins, and
  // resume at a place that only they name.
  if (handler != "__C_specific_handler")
    return;
  uint32_t scope = more + 8;
  for (uint32_t n = read32le(&d[more + 4]); n && scope + 16 <= d.size();
       --n, scope += 16) {
    std::optional<Place> end = getPlace(info.first, scope + 4);
    std::optional<Place> resume = getPlace(info.first, scope + 12);
    if (CodeSection *to = end ? getCode(end->first) : nullptr)
      to->checks.push_back(end->second);
    if (CodeSection *to = resume ? getCode(resume->first) : nullptr)
      to->anchors.push_back(resume->second);
  }
}

// Reads d, the .debug$S section n: the producer, the procedures and whether
// their frames have inline assembly, the data and labels in code, and the
// line tables.
void Walker::Object::readDebug(uint32_t n, ArrayRef<uint8_t> d) {
  if (d.size() < 4 || read32le(d.data()) != DEBUG_SECTION_MAGIC)
    return;
  Proc *proc = nullptr;
  auto codeAt = [&](uint32_t off, uint32_t &to) {
    std::optional<Place> p = getPlace(n, off);
    if (p)
      to = p->second;
    return p ? getCode(p->first) : nullptr;
  };
  for (size_t o = 4; o + 8 <= d.size();) {
    uint32_t kind = read32le(&d[o]), len = read32le(&d[o + 4]);
    o += 8;
    if (len > d.size() - o)
      return;
    uint32_t to;
    if (DebugSubsectionKind(kind) == DebugSubsectionKind::Lines) {
      DebugLinesSubsectionRef lines;
      CodeSection *cs = codeAt(o, to);
      if (Error e = lines.initialize(
              BinaryStreamReader(d.slice(o, len), endianness::little)))
        consumeError(std::move(e));
      else if (cs)
        for (const LineColumnEntry &block : lines)
          for (const LineNumberEntry &line : block.LineNumbers)
            cs->checks.push_back(to + line.Offset);
    }
    size_t end = o + len;
    if (DebugSubsectionKind(kind) != DebugSubsectionKind::Symbols)
      o = end;
    while (o + 4 <= end) {
      size_t size = read16le(&d[o]) + 2;
      if (size < 4 || size > end - o)
        break;
      CVSymbol sym(d.slice(o, size));
      switch (sym.kind()) {
      case S_COMPILE3: {
        Compile3Sym c(SymbolRecordKind::Compile3Sym);
        if (Error e = SymbolDeserializer::deserializeAs(sym, c))
          consumeError(std::move(e));
        else
          described = c.getLanguage() != SourceLanguage::Masm;
        break;
      }
      case S_GPROC32:
      case S_LPROC32:
      case S_GPROC32_ID:
      case S_LPROC32_ID: {
        ProcSym p(SymbolRecordKind::ProcSym);
        proc = nullptr;
        if (Error e = SymbolDeserializer::deserializeAs(sym, p)) {
          consumeError(std::move(e));
        } else if (CodeSection *cs = codeAt(o + p.getRelocationOffset(), to)) {
          cs->procs.push_back({{to, to + p.CodeSize}, false});
          cs->anchors.push_back(to);
          proc = &cs->procs.back();
        }
        break;
      }
      case S_FRAMEPROC: {
        FrameProcSym f(SymbolRecordKind::FrameProcSym);
        if (Error e = SymbolDeserializer::deserializeAs(sym, f))
          consumeError(std::move(e));
        else if (proc)
          proc->noInlineAsm =
              (f.Flags & FrameProcedureOptions::HasInlineAssembly) ==
              FrameProcedureOptions::None;
        proc = nullptr;
        break;
      }
      case S_LDATA32:
      case S_GDATA32:
        if (CodeSection *cs = codeAt(o + DataSym(0).getRelocationOffset(), to))
          cs->data.push_back(to);
        break;
      case S_LABEL32:
        if (CodeSection *cs = codeAt(o + LabelSym(0).getRelocationOffset(), to))
          cs->checks.push_back(to);
        break;
      default:
        break;
      }
      o += size;
    }
    o = alignTo(end, 4);
  }
}

Walker::Object::Object(ObjFile &file)
    : file(file), obj(*file.getCOFFObj()),
      sections(obj.getNumberOfSections() + 1) {
  for (uint32_t i = 0, e = obj.getNumberOfSymbols(); i < e; ++i) {
    COFFSymbolRef sym = check(obj.getSymbol(i));
    uint32_t index = i;
    i += sym.getNumberOfAuxSymbols();
    if (sym.isAbsolute()) {
      // The product in the high half is the C or the C++ compiler, or MASM.
      if (check(obj.getSymbolName(sym)) == "@comp.id") {
        marksData = true;
        compiler = (sym.getValue() >> 16) - 0x104 < 2;
      }
      continue;
    }
    bool function = sym.getComplexType() == IMAGE_SYM_DTYPE_FUNCTION;
    CodeSection *cs = !function && sym.isSectionDefinition()
                          ? nullptr
                          : getCode(sym.getSectionNumber());
    if (!cs)
      continue;
    cs->checks.push_back(sym.getValue());
    if (function) {
      cs->funcs.emplace_back(sym.getValue(), index);
      cs->anchors.push_back(sym.getValue());
    } else if (sym.getStorageClass() == IMAGE_SYM_CLASS_STATIC) {
      cs->statics.push_back(sym.getValue());
    }
  }

  for (uint32_t n = 1; n < sections.size(); ++n) {
    const coff_section *sec = check(obj.getSection(n));
    StringRef name = check(obj.getSectionName(sec));
    if (name.starts_with(".debug") || name.starts_with(".xdata"))
      continue;
    RawSection &s = section(n);
    // Each record is a section's number, a count, and that many offsets.
    if (name == ".unqRlcs") {
      unique.emplace();
      for (size_t off = 0; off + 8 <= s.data.size();) {
        uint32_t in = read32le(&s.data[off]),
                 count = read32le(&s.data[off + 4]);
        for (off += 8; count-- && off + 4 <= s.data.size(); off += 4)
          unique->insert({in, read32le(&s.data[off])});
      }
      continue;
    }
    if (name.starts_with(".pdata")) {
      for (uint32_t off = 0; off + 12 <= s.data.size(); off += 12) {
        std::optional<Place> begin = getPlace(n, off),
                             end = getPlace(n, off + 4),
                             info = getPlace(n, off + 8);
        CodeSection *cs = begin ? getCode(begin->first) : nullptr;
        if (!cs)
          continue;
        cs->anchors.push_back(begin->second);
        if (end && end->first == begin->first && begin->second < end->second) {
          cs->unwound.push_back({begin->second, end->second});
          cs->checks.push_back(end->second);
        }
        if (info)
          readUnwind(*info, begin->second, *cs);
      }
      continue;
    }
    // A word that holds the address of a place in code names that place. A
    // field that holds a distance to one does once its origin is known.
    for (const coff_relocation *rel : s.rels) {
      unsigned width = getWidth(rel->Type);
      if (uint64_t(rel->VirtualAddress) + width > s.data.size())
        continue;
      const uint8_t *field = &s.data[rel->VirtualAddress];
      bool word = rel->Type == IMAGE_REL_AMD64_ADDR64 ||
                  rel->Type == IMAGE_REL_AMD64_ADDR32 ||
                  rel->Type == IMAGE_REL_AMD64_ADDR32NB;
      if (!word && rel->Type != IMAGE_REL_AMD64_REL32)
        continue;
      std::optional<Place> to =
          getPlace(*rel, width == 8 ? read64le(field) : read32le(field));
      CodeSection *cs = to ? getCode(to->first) : nullptr;
      if (cs && word)
        cs->weak.push_back(to->second);
      else if (cs)
        relative.push_back({{n, rel->VirtualAddress}, *to});
      if (cs && !word && n != to->first && getCode(n))
        cs->named.push_back({to->second, {n, rel->VirtualAddress}});
    }
  }

  for (SectionChunk *c : file.getDebugChunks())
    if (c->getSectionName() == ".debug$S")
      readDebug(c->getSectionNumber(), c->getContents());
  taken = getTakenFunctions(file.symtab.ctx, &file);

  for (auto &[n, cs] : code) {
    llvm::sort(cs.funcs);
    llvm::sort(cs.named);
    for (std::vector<uint32_t> *v :
         {&cs.anchors, &cs.weak, &cs.checks, &cs.statics, &cs.data})
      llvm::sort(*v);
  }
}

//===----------------------------------------------------------------------===//
// The walk of one function.
//===----------------------------------------------------------------------===//

// Returns whether every byte of code is a legacy prefix in allowed, none of
// them twice.
static bool hasOnlyPrefixes(ArrayRef<uint8_t> code, StringRef allowed) {
  for (size_t i = 0; i != code.size(); ++i)
    if (!allowed.contains(char(code[i])) ||
        is_contained(code.take_front(i), code[i]))
      return false;
  return true;
}

// Classifies the REL32 on field f of the instruction code, which is decoded.
// The form is read from the instruction's first byte on, so that what precedes
// the field cannot be the end of another instruction's encoding.
static Site classifySite(ArrayRef<uint8_t> code, const InsnField &f) {
  auto is = [](bool canonical, SiteKind kind) {
    return canonical ? Site(kind) : Site(SiteKind::Refused, nonCanonical);
  };
  size_t end = f.offset + 4;
  if (f.branch) {
    // call, jmp or jcc with a 32-bit offset, after a branch hint, bnd, an
    // address size, which the processor ignores, and REX.
    size_t op = getBranchOpcode(code, f);
    bool known = op + 1 == f.offset ? code[op] == 0xe8 || code[op] == 0xe9
                                    : (code[op + 1] & 0xf0) == 0x80;
    op -= op && (code[op - 1] & 0xf0) == 0x40;
    return is(known && end == code.size() &&
                  hasOnlyPrefixes(code.take_front(op), "\x2e\x3e\x67\xf2"),
              SiteKind::Branch);
  }
  if (!f.pcRelative || f.offset < 2 || (code[f.offset - 1] & 0xc7) != 0x05)
    return SiteKind::Operand;
  // The opcode, after REX or REX2 of the first opcode map.
  size_t op = f.offset - 2, first = op;
  bool wide = false;
  if (op >= 2 && code[op - 2] == 0xd5 && !(code[op - 1] & 0x80)) {
    wide = code[op - 1] & 0x08;
    first = op - 2;
  } else if (op && (code[op - 1] & 0xf0) == 0x40) {
    wide = code[op - 1] & 0x08;
    first = op - 1;
  }
  ArrayRef<uint8_t> prefixes = code.take_front(first);
  if (!llvm::all_of(prefixes, isLegacyPrefix))
    return SiteKind::Operand;
  unsigned reg = (code[f.offset - 1] >> 3) & 7;
  // lea, with no address size and no segment. One of 64 bits has room for a
  // load of the address.
  if (code[op] == 0x8d)
    return is(end == code.size() && hasOnlyPrefixes(prefixes, "\x66\xf2\xf3"),
              wide ? SiteKind::Address : SiteKind::Operand);
  // call or jmp through memory, after notrack, a branch hint or bnd.
  if (code[op] == 0xff && (reg == 2 || reg == 4))
    return is(end == code.size() && hasOnlyPrefixes(prefixes, "\x2e\x3e\xf2"),
              SiteKind::Operand);
  return SiteKind::Operand;
}

namespace {
struct FunctionWalk {
  Walker::Object &o;
  const CodeSection &cs;
  X86Decoder &dec;
  uint32_t n;
  Extent x;
  // Whether each instruction begins where the one before it ends.
  bool linear;
  RawSection &sec;
  FunctionWalk(Walker::Object &o, const CodeSection &cs, X86Decoder &dec,
               uint32_t n, Extent x, bool linear)
      : o(o), cs(cs), dec(dec), n(n), x(x), linear(linear), sec(o.section(n)) {}

  enum : uint8_t { Free, Start, Inside, Data, Padding };
  std::vector<uint8_t> state;
  SmallVector<uint32_t, 16> work;
  // The places in data that an instruction addresses relative to rip, any of
  // which may be the origin of a table of distances, and those read as one.
  std::set<Place> bases, tables;
  DenseMap<uint32_t, Site> found;
  // The targets in other sections of the branches decoded.
  SmallVector<Place, 4> proven;
  const char *failure = nullptr;

  uint8_t &at(uint32_t off) { return state[off - x.begin]; }
  void fail(const char *why) {
    if (!failure)
      failure = why;
  }
  bool isFree(uint32_t off, unsigned size) {
    return llvm::all_of(ArrayRef(&at(off), size),
                        [](uint8_t s) { return s == Free; });
  }

  // Marks the bytes from each of the places in from to the next symbol or
  // label as data, or to a proven instruction start, which may be the place
  // itself.
  void markData(const std::vector<uint32_t> &from) {
    for (auto it = llvm::lower_bound(from, x.begin);
         it != from.end() && *it < x.end; ++it) {
      uint32_t end = x.end;
      for (const std::vector<uint32_t> *v : {&cs.checks, &from})
        if (auto next = llvm::upper_bound(*v, *it); next != v->end())
          end = std::min(end, *next);
      if (auto next = llvm::lower_bound(cs.anchors, *it);
          next != cs.anchors.end())
        end = std::min(end, *next);
      std::fill(&at(*it), &at(*it) + (end - *it), Data);
    }
  }

  // Checks the relocations on the instruction at off against its fields, and
  // classifies each REL32. Returns whether a relocation gives the target of
  // the instruction's branch.
  bool visit(uint32_t off, const Insn &insn) {
    ArrayRef<uint8_t> code = sec.data.slice(off, insn.size);
    bool relocated = false;
    for (auto it = sec.from(off < 7 ? 0 : off - 7);
         it != sec.rels.end() && (*it)->VirtualAddress < off + insn.size;
         ++it) {
      const coff_relocation &rel = **it;
      unsigned width = getWidth(rel.Type);
      if (rel.Type == IMAGE_REL_AMD64_ABSOLUTE ||
          rel.VirtualAddress + width <= off)
        continue;
      const InsnField *f = llvm::find_if(insn.fields, [&](const InsnField &f) {
        return off + f.offset == rel.VirtualAddress && f.size == width;
      });
      if (f == insn.fields.end()) {
        fail(offOperand);
        continue;
      }
      if (!isRel32(rel.Type))
        continue;
      std::optional<Place> to =
          o.getPlace(rel, read32le(&sec.data[rel.VirtualAddress]));
      // A relocated branch's field does not hold its target.
      relocated |= f->branch;
      if (f->branch && to && to->first != n)
        proven.push_back(*to);
      if (to && f->pcRelative && rel.Type == IMAGE_REL_AMD64_REL32 &&
          f->offset + 4u == insn.size)
        bases.insert(*to);
      Site &s = found[rel.VirtualAddress] = classifySite(code, *f);
      s.before = rel.VirtualAddress - off;
    }
    return relocated;
  }

  // Decodes from off, a place that execution reaches, to the end of its path.
  // A weak start may be in data, where it is no start.
  void walk(uint32_t off, bool weak = false) {
    if (!x.contains(off))
      return;
    if (at(off) == Data && !weak)
      fail(overlap);
    while (off < x.end && !failure) {
      uint8_t s = at(off);
      if (s == Data && linear) {
        ++off;
        continue;
      }
      // Marked data ends a path, as an instruction decoded before does.
      if (s == Start || s == Data)
        return;
      if (s == Inside)
        return fail(overlap);
      Insn insn;
      if (!dec.decode(sec.data.slice(off, x.end - off), off, insn))
        return fail(unknown);
      if (!isFree(off, insn.size))
        return fail(overlap);
      std::fill(&at(off), &at(off) + insn.size, Inside);
      at(off) = Start;
      if (!visit(off, insn) && insn.target)
        work.push_back(*insn.target);
      if (insn.ends && !linear)
        return;
      off += insn.size;
    }
  }

  void drain() {
    while (!work.empty())
      walk(work.pop_back_val());
  }

  // Follows the table of distances at base, a place in data: each entry in
  // turn is a REL32 whose field holds the distance from the base to a place
  // in code. The table ends where another begins, or with the entries.
  void walkTable(Place base) {
    auto it =
        llvm::lower_bound(o.relative, base, [](const RelativeRel &r, Place p) {
          return r.at < p;
        });
    for (Place at = base; it != o.relative.end() && it->at == at;
         ++it, at.second += 4) {
      if (at != base && bases.count(at))
        return;
      if (it->to.first != n)
        continue;
      walk(it->to.second - 4 - (at.second - base.second), true);
      drain();
    }
  }

  void run() {
    state.assign(x.end - x.begin, Free);
    if (o.marksData)
      markData(cs.statics);
    markData(cs.data);

    walk(x.begin);
    drain();
    for (auto it = llvm::lower_bound(cs.anchors, x.begin);
         it != cs.anchors.end() && *it < x.end; ++it) {
      walk(*it);
      drain();
    }
    // Then what only data names: the words that hold an address, and the
    // tables in the order of their places, since the code a table reaches may
    // address the next.
    for (auto it = llvm::lower_bound(cs.weak, x.begin);
         it != cs.weak.end() && *it < x.end; ++it) {
      walk(*it, true);
      drain();
    }
    for (auto it = bases.begin(); it != bases.end();
         it = llvm::find_if(bases, [&](Place p) { return !tables.count(p); })) {
      tables.insert(*it);
      walkTable(*it);
    }

    // What no path reached is padding: int3 and no-operations, with no
    // relocation.
    for (uint32_t off = x.begin; off < x.end && !failure;) {
      if (at(off) != Free) {
        ++off;
        continue;
      }
      Insn insn;
      ArrayRef<uint8_t> code = sec.data.slice(off, x.end - off);
      if (!dec.decode(code, off, insn) || !isFree(off, insn.size))
        return fail(unexplained);
      auto it = sec.from(off < 7 ? 0 : off - 7);
      while (it != sec.rels.end() &&
             (*it)->VirtualAddress + getWidth((*it)->Type) <= off)
        ++it;
      code = code.take_front(insn.size);
      while (code.size() > 1 && (code[0] == 0x66 || code[0] == 0x2e))
        code = code.drop_front();
      if ((it != sec.rels.end() && (*it)->VirtualAddress < off + insn.size) ||
          !(code.size() == 1 ? code[0] == 0x90 || code[0] == 0xcc
                             : code[0] == 0x0f && code[1] == 0x1f))
        return fail(unexplained);
      std::fill(&at(off), &at(off) + insn.size, Padding);
      off += insn.size;
    }
    for (auto it = llvm::upper_bound(cs.checks, x.begin);
         it != cs.checks.end() && *it < x.end; ++it)
      if (at(*it) == Inside)
        fail(overlap);
  }
};
} // namespace

// Returns the extent of the function that holds off of the code section n:
// its unwind range, else its CodeView procedure, else from its symbol to the
// next function's. Sets in site what each site of the function begins as.
std::optional<Extent> Walker::Object::getExtent(uint32_t n, uint32_t off,
                                                Site &site) {
  CodeSection *cs = getCode(n);
  std::optional<Extent> x;
  for (const Extent &e : cs->unwound)
    if (e.contains(off))
      x = e;
  for (const Proc &p : cs->procs)
    if (!x && p.contains(off))
      x = p;
  auto next = llvm::upper_bound(cs->funcs, std::pair(off, UINT32_MAX));
  if (!x && next != cs->funcs.begin())
    x = Extent{std::prev(next)->first,
               next == cs->funcs.end() ? UINT32_MAX : next->first};
  site = Site(SiteKind::Refused, unreached);
  site.how = ", decoded from proven starts";
  if (next != cs->funcs.begin())
    site.function =
        check(obj.getSymbolName(check(obj.getSymbol(std::prev(next)->second))));
  if (x)
    x->end = std::min<uint64_t>(x->end, section(n).data.size());
  if (!x || x->begin >= x->end)
    return std::nullopt;
  site.linear = compiler;
  for (const Proc &p : cs->procs)
    site.linear |= described && p.noInlineAsm && p.contains(x->begin);
  if (site.linear)
    site.how = ", decoded from the function's start";
  return x;
}

// Decodes the function at x of section n and gives every REL32 of it its
// answer. Returns whether the function is accepted; the branches of one that
// is, into other sections, then prove their targets.
bool Walker::Object::walk(uint32_t n, Extent x, Site site) {
  CodeSection *cs = getCode(n);
  RawSection &sec = section(n);
  X86Decoder *dec = X86Decoder::get();
  if (!dec)
    return false;
  FunctionWalk fw(*this, *cs, *dec, n, x, site.linear);
  fw.run();

  for (auto it = sec.from(x.begin);
       it != sec.rels.end() && (*it)->VirtualAddress < x.end; ++it) {
    uint32_t at = (*it)->VirtualAddress;
    if (!isRel32((*it)->Type) || at + 4 > x.end)
      continue;
    Site s = site;
    bool free = fw.isFree(at, 4);
    if (fw.failure)
      s.reason = fw.failure == unexplained && free ? unreached : fw.failure;
    else if (auto f = fw.found.find(at); f != fw.found.end())
      s.kind = f->second.kind, s.reason = f->second.reason,
      s.before = f->second.before;
    else if (llvm::all_of(ArrayRef(&fw.at(at), 4),
                          [](uint8_t b) { return b == FunctionWalk::Data; }))
      s.kind = SiteKind::Data, s.reason = nullptr;
    else
      s.reason = offOperand;
    cs->sites[at] = s;
  }
  if (fw.failure)
    return false;
  for (Place to : fw.proven)
    if (CodeSection *other = getCode(to.first))
      other->anchors.insert(llvm::upper_bound(other->anchors, to.second),
                            to.second);
  return true;
}

bool lld::coff::isRipLea64(ArrayRef<uint8_t> c, size_t o) {
  return o >= 3 && o + 4 <= c.size() && c[o - 2] == 0x8d &&
         (c[o - 1] & 0xc7) == 0x05 &&
         ((c[o - 3] & 0xf8) == 0x48 ||
          (o >= 4 && c[o - 4] == 0xd5 && (c[o - 3] & 0x88) == 0x08));
}

Site Walker::Object::classify(uint32_t n, const coff_relocation &rel) {
  uint32_t off = rel.VirtualAddress;
  CodeSection *cs = getCode(n);
  if (!cs)
    return SiteKind::Data;

  // A reference to a symbol that the object types as a function is a branch
  // where the object lists the sites that take an address and not this one,
  // or lists the functions whose address it takes and not this one, in code
  // that is known to be a compiler's alone, since no table lists what inline
  // assembly does. A listed site takes the address, and has room for a load
  // of it if it is a lea of 64 bits. A decode never answers for such a site.
  Site site;
  if (check(obj.getSymbol(rel.SymbolTableIndex)).getComplexType() ==
          IMAGE_SYM_DTYPE_FUNCTION &&
      (unique || (taken && (compiler || described) &&
                  !taken->count(file.getSymbol(rel.SymbolTableIndex)))) &&
      (getExtent(n, off, site), unique || site.linear)) {
    // A lea behind REX2 is told from one behind REX.W only where no 0xD5
    // can be the byte before the REX.W.
    ArrayRef<uint8_t> c = section(n).data;
    bool rex2 = off >= 4 && c[off - 4] == 0xd5;
    site.kind = !unique || !unique->count({n, off})   ? SiteKind::Branch
                : !isRipLea64(c, off)                 ? SiteKind::Operand
                : rex2 && (c[off - 3] & 0xf8) == 0x48 ? SiteKind::Refused
                                                      : SiteKind::Address;
    site.before = 3 + rex2;
    site.reason = site.kind == SiteKind::Refused
                      ? "an encoding that is a lea behind REX2 or one behind "
                        "REX.W after a 0xD5"
                      : nullptr;
    site.how = ", by the object's tables";
    return site;
  }
  if (auto it = cs->sites.find(off); it != cs->sites.end())
    return it->second;

  // The site's function is decoded with every function whose relocations
  // name a place in it that nothing proves yet, and so on from those: a
  // branch that one of them proves may be the only way to that place, as it
  // is to the cold part of a function in another section. Each is decoded once
  // the others that are accepted have given their targets, until no more are
  // accepted.
  struct Member {
    uint32_t n;
    Extent x;
    Site site;
    bool accepted;
  };
  SmallVector<Member, 4> group;
  SmallVector<Place, 4> todo{{n, off}};
  while (!todo.empty()) {
    Place at = todo.pop_back_val();
    CodeSection *in = getCode(at.first);
    std::optional<Extent> x = getExtent(at.first, at.second, site);
    if (!x || !in->walked.insert(x->begin).second)
      continue;
    group.push_back({at.first, *x, site, false});
    for (auto it = llvm::lower_bound(in->named, std::pair(x->begin, Place()));
         it != in->named.end() && it->first < x->end; ++it)
      if (!llvm::binary_search(in->anchors, it->first))
        todo.push_back(it->second);
  }
  for (bool more = true; more;) {
    more = false;
    for (Member &m : group)
      if (!m.accepted && walk(m.n, m.x, m.site))
        m.accepted = more = true;
  }
  getExtent(n, off, site);
  return cs->sites.try_emplace(off, site).first->second;
}

Walker::Walker() = default;
Walker::~Walker() = default;

Site Walker::classify(const SectionChunk &sc, const coff_relocation &rel) {
  // An ARM64 relocation's type states the form of its instruction.
  if (sc.getArch() == Triple::aarch64) {
    switch (rel.Type) {
    case IMAGE_REL_ARM64_BRANCH26:
    case IMAGE_REL_ARM64_BRANCH19:
    case IMAGE_REL_ARM64_BRANCH14:
      return SiteKind::Branch;
    case IMAGE_REL_ARM64_PAGEBASE_REL21:
    case IMAGE_REL_ARM64_PAGEOFFSET_12A:
      return SiteKind::Address;
    default:
      return SiteKind::Operand;
    }
  }
  if (sc.getArch() != Triple::x86_64 || !isRel32(rel.Type))
    return SiteKind::Operand;
  Object *o;
  {
    std::lock_guard<std::mutex> lock(mu);
    auto it = objects.find(sc.file);
    o = it == objects.end() ? nullptr : it->second.get();
  }
  if (!o) {
    // Another thread's object for the file, made first, stays.
    auto made = std::make_unique<Object>(*sc.file);
    std::lock_guard<std::mutex> lock(mu);
    o = objects.try_emplace(sc.file, std::move(made)).first->second.get();
  }
  return o->classify(sc.getSectionNumber(), rel);
}
