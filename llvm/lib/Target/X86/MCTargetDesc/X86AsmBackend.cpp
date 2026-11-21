//===-- X86AsmBackend.cpp - X86 Assembler Backend -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86EncodingOptimization.h"
#include "MCTargetDesc/X86FixupKinds.h"
#include "MCTargetDesc/X86MCAsmInfo.h"
#include "MCTargetDesc/X86MCOptions.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCELFStreamer.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCLFIRewriter.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
/// A wrapper for holding a mask of the values from X86::AlignBranchBoundaryKind
class X86AlignBranchKind {
private:
  uint8_t AlignBranchKind = 0;

public:
  X86AlignBranchKind() = default;
  explicit X86AlignBranchKind(StringRef Val) {
    SmallVector<StringRef, 6> BranchTypes;
    Val.split(BranchTypes, '+', -1, false);
    for (auto BranchType : BranchTypes) {
      if (BranchType == "fused")
        addKind(X86::AlignBranchFused);
      else if (BranchType == "jcc")
        addKind(X86::AlignBranchJcc);
      else if (BranchType == "jmp")
        addKind(X86::AlignBranchJmp);
      else if (BranchType == "call")
        addKind(X86::AlignBranchCall);
      else if (BranchType == "ret")
        addKind(X86::AlignBranchRet);
      else if (BranchType == "indirect")
        addKind(X86::AlignBranchIndirect);
      else {
        errs() << "invalid argument " << BranchType.str()
               << " to -x86-align-branch=; each element must be one of: fused, "
                  "jcc, jmp, call, ret, indirect.(plus separated)\n";
      }
    }
  }

  operator uint8_t() const { return AlignBranchKind; }
  void addKind(X86::AlignBranchBoundaryKind Value) { AlignBranchKind |= Value; }
};

class X86AsmBackend : public MCAsmBackend {
  const X86MCOptions &CLOpts;
  const MCSubtargetInfo &STI;
  std::unique_ptr<const MCInstrInfo> MCII;
  X86AlignBranchKind AlignBranchType;
  Align AlignBoundary;
  unsigned TargetPrefixMax = 0;

  MCInst PrevInst;
  unsigned PrevInstOpcode = 0;
  bool PrefixEndsBundleLock = false;
  MCBoundaryAlignFragment *PendingBA = nullptr;
  std::pair<MCFragment *, size_t> PrevInstPosition;

  bool isMacroFused(const MCInst &Cmp, const MCInst &Jcc) const;
  bool needAlign(const MCInst &Inst) const;
  bool canPadBranches(MCObjectStreamer &OS) const;
  bool canPadInst(const MCInst &Inst, MCObjectStreamer &OS) const;
  bool canPadHere(MCObjectStreamer &OS) const;
  void emitInstructionBeginBundle(MCObjectStreamer &OS);
  void emitInstructionEndBundle(MCObjectStreamer &OS);

public:
  X86AsmBackend(const X86MCOptions &CLOpts, const Target &T,
                const MCSubtargetInfo &STI)
      : MCAsmBackend(llvm::endianness::little), CLOpts(CLOpts), STI(STI),
        MCII(T.createMCInstrInfo()) {
    if (CLOpts.branches_within_32B_boundaries) {
      // At the moment, this defaults to aligning fused branches, unconditional
      // jumps, and (unfused) conditional jumps with nops.  Both the
      // instructions aligned and the alignment method (nop vs prefix) may
      // change in the future.
      AlignBoundary = assumeAligned(32);
      AlignBranchType.addKind(X86::AlignBranchFused);
      AlignBranchType.addKind(X86::AlignBranchJcc);
      AlignBranchType.addKind(X86::AlignBranchJmp);
    }
    // Allow overriding defaults set by main flag
    if (CLOpts.align_branch_boundary)
      AlignBoundary = assumeAligned(*CLOpts.align_branch_boundary);
    if (CLOpts.align_branch)
      AlignBranchType = X86AlignBranchKind(*CLOpts.align_branch);
    if (CLOpts.pad_max_prefix_size)
      TargetPrefixMax = *CLOpts.pad_max_prefix_size;

    AllowAutoPadding =
        AlignBoundary != Align(1) && AlignBranchType != X86::AlignBranchNone;
    AllowEnhancedRelaxation =
        AllowAutoPadding && TargetPrefixMax != 0 && CLOpts.pad_for_branch_align;
    AllowBundling = true;
  }

  const X86MCOptions &getCLOpts() const { return CLOpts; }

  // The streamer frees the fragments these point into.
  void reset() override {
    PrevInst = MCInst();
    PrevInstOpcode = 0;
    PrefixEndsBundleLock = false;
    PendingBA = nullptr;
    PrevInstPosition = {};
  }

  void emitFusedSiteBegin(MCObjectStreamer &OS, const MCInst &Inst,
                          const MCSubtargetInfo &STI, unsigned Part,
                          MCSymbol *Marker);
  void emitInstructionBegin(MCObjectStreamer &OS, const MCInst &Inst,
                            const MCSubtargetInfo &STI);
  void emitInstructionEnd(MCObjectStreamer &OS, const MCInst &Inst,
                          unsigned Part = X86_MC::FSP_None);


  std::optional<MCFixupKind> getFixupKind(StringRef Name) const override;

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override;

  std::optional<bool> evaluateFixup(const MCFragment &, MCFixup &, MCValue &,
                                    uint64_t &) override;
  void applyFixup(const MCFragment &, const MCFixup &, const MCValue &Target,
                  uint8_t *Data, uint64_t Value, bool IsResolved) override;

  bool mayNeedRelaxation(unsigned Opcode, ArrayRef<MCOperand> Operands,
                         const MCSubtargetInfo &STI) const override;

  bool fixupNeedsRelaxationAdvanced(const MCFragment &, const MCFixup &,
                                    const MCValue &, uint64_t,
                                    bool) const override;

  void relaxInstruction(MCInst &Inst,
                        const MCSubtargetInfo &STI) const override;

  bool relaxAlign(MCFragment &F, unsigned &Size) override;
  bool keepLoop(MCFragment &F, bool Moves, unsigned &Size);
  void stateAlign(MCFragment &F, Align Alignment, unsigned Mode) const;
  bool compacts(const MCSection &Sec) const;

  bool padInstructionViaRelaxation(MCFragment &RF, MCCodeEmitter &Emitter,
                                   unsigned &RemainingSize) const;

  bool padInstructionViaPrefix(MCFragment &RF, MCCodeEmitter &Emitter,
                               unsigned &RemainingSize) const;

  bool padInstructionEncoding(MCFragment &RF, MCCodeEmitter &Emitter,
                              unsigned &RemainingSize) const;

  bool finishLayout() const override;

  bool padInstsBackward(SmallVectorImpl<MCFragment *> &Relaxable,
                        unsigned &RemainingSize) const;
  bool foldBundlePad(const MCAssembler &Asm, MCBoundaryAlignFragment &BF,
                     SmallVectorImpl<MCFragment *> &Relaxable) const;
  bool optimizeBundleNops(const MCAssembler &Asm) const;

  unsigned getMaximumNopSize(const MCSubtargetInfo &STI) const override;

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};
} // end anonymous namespace

static bool isRelaxableBranch(unsigned Opcode) {
  return Opcode == X86::JCC_1 || Opcode == X86::JMP_1;
}

static unsigned getRelaxedOpcodeBranch(unsigned Opcode,
                                       bool Is16BitMode = false) {
  switch (Opcode) {
  default:
    llvm_unreachable("invalid opcode for branch");
  case X86::JCC_1:
    return (Is16BitMode) ? X86::JCC_2 : X86::JCC_4;
  case X86::JMP_1:
    return (Is16BitMode) ? X86::JMP_2 : X86::JMP_4;
  }
}

static unsigned getRelaxedOpcode(const MCInst &MI, bool Is16BitMode) {
  unsigned Opcode = MI.getOpcode();
  return isRelaxableBranch(Opcode) ? getRelaxedOpcodeBranch(Opcode, Is16BitMode)
                                   : X86::getOpcodeForLongImmediateForm(Opcode);
}

static X86::CondCode getCondFromBranch(const MCInst &MI,
                                       const MCInstrInfo &MCII) {
  unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
  default:
    return X86::COND_INVALID;
  case X86::JCC_1: {
    const MCInstrDesc &Desc = MCII.get(Opcode);
    return static_cast<X86::CondCode>(
        MI.getOperand(Desc.getNumOperands() - 1).getImm());
  }
  }
}

static X86::SecondMacroFusionInstKind
classifySecondInstInMacroFusion(const MCInst &MI, const MCInstrInfo &MCII) {
  X86::CondCode CC = getCondFromBranch(MI, MCII);
  return classifySecondCondCodeInMacroFusion(CC);
}

/// Check if the instruction uses RIP relative addressing.
static bool isRIPRelative(const MCInst &MI, const MCInstrInfo &MCII) {
  const MCInstrDesc &Desc = MCII.get(MI.getOpcode());
  int MemoryOperand = X86II::getMemoryOperandIdx(Desc);
  if (MemoryOperand < 0)
    return false;
  unsigned BaseRegNum = MemoryOperand + X86::AddrBaseReg;
  MCRegister BaseReg = MI.getOperand(BaseRegNum).getReg();
  return (BaseReg == X86::RIP);
}

/// Check if the instruction is a prefix.
static bool isPrefix(unsigned Opcode, const MCInstrInfo &MCII) {
  return X86II::isPrefix(MCII.get(Opcode).TSFlags);
}

/// Check if the instruction is valid as the first instruction in macro fusion.
static bool isFirstMacroFusibleInst(const MCInst &Inst,
                                    const MCInstrInfo &MCII) {
  // An Intel instruction with RIP relative addressing is not macro fusible.
  if (isRIPRelative(Inst, MCII))
    return false;
  X86::FirstMacroFusionInstKind FIK =
      X86::classifyFirstOpcodeInMacroFusion(Inst.getOpcode());
  return FIK != X86::FirstMacroFusionInstKind::Invalid;
}

/// Check if the two instructions will be macro-fused on the target cpu.
bool X86AsmBackend::isMacroFused(const MCInst &Cmp, const MCInst &Jcc) const {
  const MCInstrDesc &InstDesc = MCII->get(Jcc.getOpcode());
  if (!InstDesc.isConditionalBranch())
    return false;
  if (!isFirstMacroFusibleInst(Cmp, *MCII))
    return false;
  const X86::FirstMacroFusionInstKind CmpKind =
      X86::classifyFirstOpcodeInMacroFusion(Cmp.getOpcode());
  const X86::SecondMacroFusionInstKind BranchKind =
      classifySecondInstInMacroFusion(Jcc, *MCII);
  return X86::isMacroFused(CmpKind, BranchKind);
}

/// Check if the instruction has a variant symbol operand.
static bool hasVariantSymbol(const MCInst &MI) {
  for (auto &Operand : MI) {
    if (!Operand.isExpr())
      continue;
    const MCExpr &Expr = *Operand.getExpr();
    if (Expr.getKind() == MCExpr::SymbolRef &&
        cast<MCSymbolRefExpr>(&Expr)->getSpecifier())
      return true;
  }
  return false;
}

/// X86 has certain instructions which enable interrupts exactly one
/// instruction *after* the instruction which stores to SS.  Return true if the
/// given instruction may have such an interrupt delay slot.
static bool mayHaveInterruptDelaySlot(unsigned InstOpcode) {
  switch (InstOpcode) {
  case X86::POPSS16:
  case X86::POPSS32:
  case X86::STI:
    return true;

  case X86::MOV16sr:
  case X86::MOV32sr:
  case X86::MOV64sr:
  case X86::MOV16sm:
    // In fact, this is only the case if the first operand is SS. However, as
    // segment moves occur extremely rarely, this is just a minor pessimization.
    return true;
  }
  return false;
}

/// Return true if we can insert NOP or prefixes automatically before the
/// the instruction to be emitted.
bool X86AsmBackend::canPadInst(const MCInst &Inst, MCObjectStreamer &OS) const {
  if (hasVariantSymbol(Inst))
    // Linker may rewrite the instruction with variant symbol operand(e.g.
    // TLSCALL).
    return false;

  if (isPrefix(Inst.getOpcode(), *MCII))
    // If this instruction is a prefix, inserting a prefix would change
    // semantic.
    return false;

  return canPadHere(OS);
}

/// Return true if a NOP inserted where the next instruction is emitted would
/// stand between two instructions.
bool X86AsmBackend::canPadHere(MCObjectStreamer &OS) const {
  if (mayHaveInterruptDelaySlot(PrevInstOpcode))
    // If this instruction follows an interrupt enabling instruction with a one
    // instruction delay, inserting a nop would change behavior.
    return false;

  if (isPrefix(PrevInstOpcode, *MCII))
    // If this instruction follows a prefix, inserting a nop/prefix would change
    // semantic.
    return false;

  // If this instruction follows any data, there is no clear instruction
  // boundary, inserting a nop/prefix would change semantic.
  auto Offset = OS.getCurFragSize();
  if (Offset && (OS.getCurrentFragment() != PrevInstPosition.first ||
                 Offset != PrevInstPosition.second))
    return false;

  return true;
}

bool X86AsmBackend::canPadBranches(MCObjectStreamer &OS) const {
  if (!OS.getAllowAutoPadding())
    return false;
  assert(allowAutoPadding() && "incorrect initialization!");

  // We only pad in text section.
  if (!OS.getCurrentSectionOnly()->isText())
    return false;

  // Branches only need to be aligned in 32-bit or 64-bit mode.
  if (!(STI.hasFeature(X86::Is64Bit) || STI.hasFeature(X86::Is32Bit)))
    return false;

  return true;
}

/// Check if the instruction operand needs to be aligned.
bool X86AsmBackend::needAlign(const MCInst &Inst) const {
  const MCInstrDesc &Desc = MCII->get(Inst.getOpcode());
  return (Desc.isConditionalBranch() &&
          (AlignBranchType & X86::AlignBranchJcc)) ||
         (Desc.isUnconditionalBranch() &&
          (AlignBranchType & X86::AlignBranchJmp)) ||
         (Desc.isCall() && (AlignBranchType & X86::AlignBranchCall)) ||
         (Desc.isReturn() && (AlignBranchType & X86::AlignBranchRet)) ||
         (Desc.isIndirectBranch() &&
          (AlignBranchType & X86::AlignBranchIndirect));
}

void X86_MC::emitInstruction(MCObjectStreamer &S, const MCInst &Inst,
                             const MCSubtargetInfo &STI, unsigned Part,
                             MCSymbol *Marker) {
  auto &Backend = static_cast<X86AsmBackend &>(S.getAssembler().getBackend());
  bool AutoPadding = S.getAllowAutoPadding();
  if (LLVM_LIKELY(!AutoPadding && !Backend.getCLOpts().pad_for_align)) {
    S.MCObjectStreamer::emitInstruction(Inst, STI);
    return;
  }

  // Run the LFI rewriter outside of emitInstructionBegin/End so that nested
  // instructions or bundle_lock/unlock directives do not corrupt the Begin/End
  // bookkeeping for the original instruction.
  if (S.getLFIRewriter() && S.getLFIRewriter()->rewriteInst(Inst, S, STI))
    return;

  if (S.getAssembler().isBundlingEnabled())
    Part = FSP_None;
  if (Part)
    Backend.emitFusedSiteBegin(S, Inst, STI, Part, Marker);
  else
    Backend.emitInstructionBegin(S, Inst, STI);
  S.MCObjectStreamer::emitInstruction(Inst, STI);
  Backend.emitInstructionEnd(S, Inst, Part);
}

/// Begin an instruction of a fused site. The linker rewrites the site in
/// place, so no padding enters it and none lengthens one of its
/// instructions: a BoundaryAlignFragment for its call or jmp goes before the
/// whole site, and \p Marker, where the site has one, follows it.
void X86AsmBackend::emitFusedSiteBegin(MCObjectStreamer &OS, const MCInst &Inst,
                                       const MCSubtargetInfo &STI,
                                       unsigned Part, MCSymbol *Marker) {
  OS.getCurrentFragment()->setAllowAutoPadding(false);
  if (!(Part & X86_MC::FSP_First))
    return;
  PendingBA = nullptr;
  // A site that begins with the load of its target is a call's. One that
  // begins with a test ends with a jcc, which fuses with the test of a
  // register and not with the compare of a pointer in memory.
  unsigned Type = 0;
  switch (Inst.getOpcode()) {
  case X86::MOV64rm:
    Type = X86::AlignBranchCall;
    break;
  case X86::TEST64rr:
    Type = X86::AlignBranchFused;
    break;
  case X86::CMP64mi8:
  case X86::CMP64mi32:
    Type = X86::AlignBranchJcc;
    break;
  }
  bool NeedAlign = Type ? AlignBranchType & Type : needAlign(Inst);
  if (!NeedAlign || !canPadBranches(OS) || !canPadHere(OS))
    return;
  PendingBA =
      OS.newSpecialFragment<MCBoundaryAlignFragment>(AlignBoundary, STI);
  if (Marker) {
    Marker->setFragment(OS.getCurrentFragment());
    Marker->setOffset(0);
  }
}

/// Open a MCBoundaryAlignFragment for the upcoming instruction so that layout
/// can pad it into the next bundle. Within .bundle_lock the group's fragment
/// already covers it.
void X86AsmBackend::emitInstructionBeginBundle(MCObjectStreamer &OS) {
  assert(Asm->isBundlingEnabled());

  // The prefix stays in the group while this instruction gets its own
  // fragment, so padding may land between the two.
  if (PrefixEndsBundleLock && OS.getCurrentFragment() != PrevInstPosition.first)
    getContext().reportError(OS.getStartTokLoc(),
                             "instruction prefix cannot be the last "
                             "instruction of a .bundle_lock group");

  if (OS.isBundleLocked())
    return;
  // A pending fragment means the previous MCInst was a prefix, which must stay
  // with this one: extend its range. Adjacency rejects a fragment left stale by
  // an intervening .bundle_lock group.
  if (PendingBA &&
      PendingBA->getLastFragment()->getNext() == OS.getCurrentFragment()) {
    PendingBA->setLastFragment(OS.getCurrentFragment());
    return;
  }
  PendingBA = OS.newSpecialFragment<MCBoundaryAlignFragment>(
      Asm->getBundleAlign(), STI);
  // We can set LastFragment now, before the instruction is emitted, as bundling
  // emits one fragment per instruction. Deferring setLastFragment to
  // post-emitInstruction would risk capturing a fragment that a subsequent
  // emitCodeAlignment repurposes in-place to FT_Align, corrupting the BA's
  // boundary range.
  PendingBA->setLastFragment(OS.getCurrentFragment());
}

/// Close the fragment opened by emitInstructionBeginBundle, unless the
/// instruction was a prefix, in which case the next one extends it.
void X86AsmBackend::emitInstructionEndBundle(MCObjectStreamer &OS) {
  assert(Asm->isBundlingEnabled());

  if (OS.isBundleLocked()) {
    PrefixEndsBundleLock = isPrefix(PrevInstOpcode, *MCII);
    return;
  }
  PrefixEndsBundleLock = false;
  assert(PendingBA && "MCBoundaryAlignFragment is expected for every "
                      "instruction if it is not bundle-locked");

  OS.getCurrentSectionOnly()->ensureMinAlignment(Asm->getBundleAlign());

  if (!isPrefix(PrevInstOpcode, *MCII))
    PendingBA = nullptr;
}

/// Insert BoundaryAlignFragment before instructions to align branches.
void X86AsmBackend::emitInstructionBegin(MCObjectStreamer &OS,
                                         const MCInst &Inst,
                                         const MCSubtargetInfo &STI) {
  bool CanPadInst = canPadInst(Inst, OS);
  if (Asm->isBundlingEnabled()) {
    emitInstructionBeginBundle(OS);
    OS.getCurrentFragment()->setAllowAutoPadding(CanPadInst);
    return;
  }
  if (CanPadInst)
    OS.getCurrentFragment()->setAllowAutoPadding(true);

  if (!canPadBranches(OS))
    return;

  // NB: PrevInst only valid if canPadBranches is true.
  if (!isMacroFused(PrevInst, Inst))
    // Macro fusion doesn't happen indeed, clear the pending.
    PendingBA = nullptr;

  // When branch padding is enabled (basically the skx102 erratum => unlikely),
  // we call canPadInst (not cheap) twice. However, in the common case, we can
  // avoid unnecessary calls to that, as this is otherwise only used for
  // relaxable fragments.
  if (!CanPadInst)
    return;

  if (PendingBA) {
    auto *NextFragment = PendingBA->getNext();
    assert(NextFragment && "NextFragment should not be null");
    if (NextFragment == OS.getCurrentFragment())
      return;
    // We eagerly create an empty fragment when inserting a fragment
    // with a variable-size tail.
    if (NextFragment->getNext() == OS.getCurrentFragment())
      return;

    // Macro fusion actually happens and there is no other fragment inserted
    // after the previous instruction.
    //
    // Do nothing here since we already inserted a BoudaryAlign fragment when
    // we met the first instruction in the fused pair and we'll tie them
    // together in emitInstructionEnd.
    //
    // Note: When there is at least one fragment, such as MCAlignFragment,
    // inserted after the previous instruction, e.g.
    //
    // \code
    //   cmp %rax %rcx
    //   .align 16
    //   je .Label0
    // \ endcode
    //
    // We will treat the JCC as a unfused branch although it may be fused
    // with the CMP.
    return;
  }

  if (needAlign(Inst) || ((AlignBranchType & X86::AlignBranchFused) &&
                          isFirstMacroFusibleInst(Inst, *MCII))) {
    // If we meet a unfused branch or the first instuction in a fusiable pair,
    // insert a BoundaryAlign fragment.
    PendingBA =
        OS.newSpecialFragment<MCBoundaryAlignFragment>(AlignBoundary, STI);
  }
}

/// Set the last fragment to be aligned for the BoundaryAlignFragment.
void X86AsmBackend::emitInstructionEnd(MCObjectStreamer &OS, const MCInst &Inst,
                                       unsigned Part) {
  // Update PrevInstOpcode here, canPadInst() reads that.
  MCFragment *CF = OS.getCurrentFragment();
  PrevInstOpcode = Inst.getOpcode();
  PrevInstPosition = std::make_pair(CF, OS.getCurFragSize());
  if (Asm->isBundlingEnabled())
    return emitInstructionEndBundle(OS);

  if (!canPadBranches(OS))
    return;

  // PrevInst is only needed if canPadBranches. Copying an MCInst isn't cheap.
  PrevInst = Inst;

  // A fused site ends the fragments its BoundaryAlign covers where it ends.
  if (!(Part ? Part & X86_MC::FSP_Last : needAlign(Inst)) || !PendingBA)
    return;

  // Tie the aligned instructions into a pending BoundaryAlign.
  PendingBA->setLastFragment(CF);
  PendingBA = nullptr;

  // We need to ensure that further data isn't added to the current
  // DataFragment, so that we can get the size of instructions later in
  // MCAssembler::relaxBoundaryAlign. The easiest way is to insert a new empty
  // DataFragment.
  OS.newFragment();

  // Update the maximum alignment on the current section if necessary.
  CF->getParent()->ensureMinAlignment(AlignBoundary);
}

std::optional<MCFixupKind> X86AsmBackend::getFixupKind(StringRef Name) const {
  if (STI.getTargetTriple().isOSBinFormatELF()) {
    unsigned Type;
    if (STI.getTargetTriple().isX86_64()) {
      Type = llvm::StringSwitch<unsigned>(Name)
#define ELF_RELOC(X, Y) .Case(#X, Y)
#include "llvm/BinaryFormat/ELFRelocs/x86_64.def"
#undef ELF_RELOC
                 .Case("BFD_RELOC_NONE", ELF::R_X86_64_NONE)
                 .Case("BFD_RELOC_8", ELF::R_X86_64_8)
                 .Case("BFD_RELOC_16", ELF::R_X86_64_16)
                 .Case("BFD_RELOC_32", ELF::R_X86_64_32)
                 .Case("BFD_RELOC_64", ELF::R_X86_64_64)
                 .Default(-1u);
    } else {
      Type = llvm::StringSwitch<unsigned>(Name)
#define ELF_RELOC(X, Y) .Case(#X, Y)
#include "llvm/BinaryFormat/ELFRelocs/i386.def"
#undef ELF_RELOC
                 .Case("BFD_RELOC_NONE", ELF::R_386_NONE)
                 .Case("BFD_RELOC_8", ELF::R_386_8)
                 .Case("BFD_RELOC_16", ELF::R_386_16)
                 .Case("BFD_RELOC_32", ELF::R_386_32)
                 .Default(-1u);
    }
    if (Type == -1u)
      return std::nullopt;
    return static_cast<MCFixupKind>(FirstLiteralRelocationKind + Type);
  }
  return MCAsmBackend::getFixupKind(Name);
}

MCFixupKindInfo X86AsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  const static MCFixupKindInfo Infos[X86::NumTargetFixupKinds] = {
      // clang-format off
      {"reloc_riprel_4byte", 0, 32, 0},
      {"reloc_riprel_4byte_movq_load", 0, 32, 0},
      {"reloc_riprel_4byte_movq_load_rex2", 0, 32, 0},
      {"reloc_riprel_4byte_relax", 0, 32, 0},
      {"reloc_riprel_4byte_relax_rex", 0, 32, 0},
      {"reloc_riprel_4byte_relax_rex2", 0, 32, 0},
      {"reloc_riprel_4byte_relax_evex", 0, 32, 0},
      {"reloc_signed_4byte", 0, 32, 0},
      {"reloc_signed_4byte_relax", 0, 32, 0},
      {"reloc_global_offset_table", 0, 32, 0},
      {"reloc_branch_4byte_pcrel", 0, 32, 0},
      {"reloc_riprel_4byte_imm1", 0, 32, 0},
      {"reloc_riprel_4byte_imm2", 0, 32, 0},
      {"reloc_riprel_4byte_imm4", 0, 32, 0},
      {"reloc_riprel_4byte_call", 0, 32, 0},
      {"reloc_riprel_4byte_jump", 0, 32, 0},
      {"reloc_riprel_4byte_jump_p1", 0, 32, 0},
      {"reloc_riprel_4byte_tail", 0, 32, 0},
      {"reloc_riprel_4byte_lea", 0, 32, 0},
      {"reloc_riprel_4byte_narrow", 0, 32, 0},
      {"reloc_riprel_4byte_test", 0, 32, 0},
      {"reloc_jump_4byte_pcrel", 0, 32, 0},
      {"reloc_jump_1byte_pcrel", 0, 8, 0},
      {"reloc_tail_4byte_pcrel", 0, 32, 0},
      // clang-format on
  };

  // Fixup kinds from .reloc directive are like R_386_NONE/R_X86_64_NONE. They
  // do not require any extra processing.
  if (mc::isRelocation(Kind))
    return {};

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);

  assert(unsigned(Kind - FirstTargetFixupKind) < X86::NumTargetFixupKinds &&
         "Invalid kind!");
  assert(Infos[Kind - FirstTargetFixupKind].Name && "Empty fixup name!");
  return Infos[Kind - FirstTargetFixupKind];
}

static unsigned getFixupKindSize(unsigned Kind) {
  switch (Kind) {
  default:
    llvm_unreachable("invalid fixup kind!");
  case FK_NONE:
  case FK_Data_leb128:
    return 0;
  case FK_SecRel_1:
  case FK_Data_1:
  case X86::reloc_jump_1byte_pcrel:
    return 1;
  case FK_SecRel_2:
  case FK_Data_2:
    return 2;
  case X86::reloc_riprel_4byte:
  case X86::reloc_riprel_4byte_imm1:
  case X86::reloc_riprel_4byte_imm2:
  case X86::reloc_riprel_4byte_imm4:
  case X86::reloc_riprel_4byte_call:
  case X86::reloc_riprel_4byte_jump:
  case X86::reloc_riprel_4byte_jump_p1:
  case X86::reloc_riprel_4byte_tail:
  case X86::reloc_riprel_4byte_lea:
  case X86::reloc_riprel_4byte_narrow:
  case X86::reloc_riprel_4byte_test:
  case X86::reloc_jump_4byte_pcrel:
  case X86::reloc_tail_4byte_pcrel:
  case X86::reloc_riprel_4byte_relax:
  case X86::reloc_riprel_4byte_relax_rex:
  case X86::reloc_riprel_4byte_relax_rex2:
  case X86::reloc_riprel_4byte_movq_load:
  case X86::reloc_riprel_4byte_movq_load_rex2:
  case X86::reloc_riprel_4byte_relax_evex:
  case X86::reloc_signed_4byte:
  case X86::reloc_signed_4byte_relax:
  case X86::reloc_global_offset_table:
  case X86::reloc_branch_4byte_pcrel:
  case FK_SecRel_4:
  case FK_Data_4:
    return 4;
  case FK_SecRel_8:
  case FK_Data_8:
    return 8;
  }
}

constexpr char GotSymName[] = "_GLOBAL_OFFSET_TABLE_";

// Adjust PC-relative fixup offsets, which are calculated from the start of the
// next instruction.
std::optional<bool> X86AsmBackend::evaluateFixup(const MCFragment &F,
                                                 MCFixup &Fixup,
                                                 MCValue &Target, uint64_t &) {
  if (Fixup.isPCRel()) {
    // The variable part of a relaxable fragment holds a jump whose width the
    // assembler chooses. On Windows Itanium and NT-POSIX its relocation states
    // that width, which the linker may change.
    if (F.getKind() == MCFragment::FT_Relaxable &&
        Fixup.getOffset() >= F.getFixedSize() && STI.hasFeature(X86::Is64Bit) &&
        STI.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment() &&
        (Fixup.getKind() == FK_Data_1 ||
         Fixup.getKind() == X86::reloc_branch_4byte_pcrel))
      Fixup = MCFixup::create(Fixup.getOffset(), Fixup.getValue(),
                              Fixup.getKind() == FK_Data_1
                                  ? X86::reloc_jump_1byte_pcrel
                                  : X86::reloc_jump_4byte_pcrel,
                              true);
    switch (Fixup.getKind()) {
    case FK_Data_1:
    case X86::reloc_jump_1byte_pcrel:
      Target.setConstant(Target.getConstant() - 1);
      break;
    case FK_Data_2:
      Target.setConstant(Target.getConstant() - 2);
      break;
    default: {
      Target.setConstant(Target.getConstant() - 4);
      auto *Add = Target.getAddSym();
      // If this is a pc-relative load off _GLOBAL_OFFSET_TABLE_:
      // leaq _GLOBAL_OFFSET_TABLE_(%rip), %r15
      // this needs to be a GOTPC32 relocation.
      if (Add && Add->getName() == GotSymName)
        Fixup = MCFixup::create(Fixup.getOffset(), Fixup.getValue(),
                                X86::reloc_global_offset_table);
    } break;
    }
  }
  // Use default handling for `Value` and `IsResolved`.
  return {};
}

void X86AsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                               const MCValue &Target, uint8_t *Data,
                               uint64_t Value, bool IsResolved) {
  // Force relocation when there is a specifier. This might be too conservative
  // - GAS doesn't emit a relocation for call local@plt; local:.
  if (Target.getSpecifier())
    IsResolved = false;
  // The field of a load of a pointer holds no offset, and its fixup is of the
  // pointer less the four bytes of the field. What the assembler added to
  // that states the load to the linker, which reads it only from a
  // relocation.
  if (int64_t States = Target.getConstant() + 4;
      IsResolved && States &&
      (Fixup.getKind() == X86::reloc_riprel_4byte_movq_load ||
       Fixup.getKind() == X86::reloc_riprel_4byte_movq_load_rex2))
    return applyFixup(F, Fixup,
                      MCValue::get(Target.getAddSym(), Target.getSubSym(), -4),
                      Data, Value - States, IsResolved);
  // The linker may change a distance the assembler resolved across code that
  // it resizes, so the object states the field. A jump of a width the
  // assembler chose keeps its relocation. An 8-bit field is the span from a
  // label at the end of its instruction, so that no opcode need be read. Any
  // other field keeps its value under a marker that names a label at its
  // target.
  // In a section the linker only compacts, a span states the field instead:
  // its instruction ends with it, or with the immediate after it. There a
  // jump in a fragment that is the linker's to resize is one whose width the
  // linker chooses, whatever its distance spans: the fragment of a jmp that
  // may be deleted is the linker's too, but that jmp is in no such section.
  bool States = F.getParent()->isLinkerRelaxable() &&
                static_cast<WinCOFFObjectWriter &>(Asm->getWriter())
                    .statesSpans(*F.getParent());
  bool IsJump = States && F.isLinkerRelaxable() &&
                F.getKind() == MCFragment::FT_Relaxable &&
                (Fixup.getKind() == X86::reloc_jump_4byte_pcrel ||
                 Fixup.getKind() == X86::reloc_tail_4byte_pcrel);
  auto StatesSpan = [&] {
    auto &W = static_cast<WinCOFFObjectWriter &>(Asm->getWriter());
    unsigned Size = getFixupKindSize(Fixup.getKind());
    if (!States || (Size != 1 && Size != 4))
      return false;
    unsigned After = Fixup.getKind() == X86::reloc_riprel_4byte_imm1   ? 1
                     : Fixup.getKind() == X86::reloc_riprel_4byte_imm2 ? 2
                     : Fixup.getKind() == X86::reloc_riprel_4byte_imm4 ? 4
                                                                       : 0;
    W.addSpan(F, Fixup, *F.getParent(),
              IsJump      ? COFF::SpanJump32
              : Size == 1 ? COFF::SpanRel8
                          : COFF::SpanRel32 + After);
    return true;
  };
  // A jump to another section whose width is the linker's keeps its
  // relocation beside the span, as does one through a pointer that the
  // linker may write as such a jump.
  if (!IsResolved && States && F.isLinkerRelaxable() &&
      (IsJump || Fixup.getKind() == X86::reloc_riprel_4byte_jump ||
       Fixup.getKind() == X86::reloc_riprel_4byte_jump_p1 ||
       Fixup.getKind() == X86::reloc_riprel_4byte_tail)) {
    IsJump = true;
    StatesSpan();
  }
  if (IsResolved && Fixup.isPCRel() &&
      (IsJump || !isPCRelFixupResolved(Target.getAddSym(), F)) &&
      !StatesSpan()) {
    switch (Fixup.getKind()) {
    case X86::reloc_jump_1byte_pcrel:
    case X86::reloc_jump_4byte_pcrel:
    case X86::reloc_tail_4byte_pcrel:
    case FK_Data_2:
      IsResolved = false;
      break;
    case FK_Data_1: {
      MCSymbol *End = getContext().createTempSymbol();
      End->setFragment(const_cast<MCFragment *>(&F));
      End->setOffset(Fixup.getOffset() + 1);
      Asm->getWriter().recordRelocation(
          F, MCFixup::create(Fixup.getOffset(), Fixup.getValue(), FK_Data_1),
          MCValue::get(Target.getAddSym(), End, Target.getConstant() + 1),
          Value);
      break;
    }
    default: {
      uint64_t Unused;
      Asm->getWriter().recordRelocation(
          F,
          MCFixup::create(Fixup.getOffset(), Fixup.getValue(),
                          getCOFFRelocFixupKind(COFF::IMAGE_REL_AMD64_SREL32)),
          Target, Unused);
      break;
    }
    }
  }
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);

  auto Kind = Fixup.getKind();
  if (mc::isRelocation(Kind))
    return;
  unsigned Size = getFixupKindSize(Kind);

  assert(Fixup.getOffset() + Size <= F.getSize() && "Invalid fixup offset!");

  // Check fixup value overflow similar to GAS (fixups emitted as RELA
  // relocations have a value of 0).
  // - Unknown signedness: the range (-2^N, 2^N) is allowed,
  //   accommodating intN_t, uintN_t, and a non-positive value type.
  // - Signed (intN_t): the range [-2^(N-1), 2^(N-1)) is allowed.
  //
  // Currently only resolved PC-relative fixups are treated as signed. GAS
  // treats more as signed (e.g. unresolved R_X86_64_32S).
  // Unresolved fixups have unknown signedness to allow `jmp foo+0xffffffff`.
  if (Size && Size < 8) {
    bool Signed = IsResolved && Fixup.isPCRel();
    uint64_t Mask = ~uint64_t(0) << (Size * 8 - (Signed ? 1 : 0));
    if ((Value & Mask) && (Signed ? (Value & Mask) != Mask : (-Value & Mask)))
      getContext().reportError(Fixup.getLoc(),
                               "value of " + Twine(int64_t(Value)) +
                                   " is too large for field of " + Twine(Size) +
                                   (Size == 1 ? " byte" : " bytes"));
  }

  for (unsigned i = 0; i != Size; ++i)
    Data[i] = uint8_t(Value >> (i * 8));
}

// Whether Sec is one whose code a Windows Itanium or NT-POSIX linker only
// compacts, whose object may state spans of it.
bool X86AsmBackend::compacts(const MCSection &Sec) const {
  return STI.hasFeature(X86::Is64Bit) &&
         STI.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment() &&
         static_cast<WinCOFFObjectWriter &>(Asm->getWriter())
             .Spans.count({&Sec, &Sec});
}

bool X86AsmBackend::mayNeedRelaxation(unsigned Opcode,
                                      ArrayRef<MCOperand> Operands,
                                      const MCSubtargetInfo &STI) const {
  unsigned SkipOperands = X86::isCCMPCC(Opcode) ? 2 : 0;
  return isRelaxableBranch(Opcode) ||
         (X86::getOpcodeForLongImmediateForm(Opcode) != Opcode &&
          Operands[Operands.size() - 1 - SkipOperands].isExpr());
}

// The size of the loop whose alignment F is, from the last alignment stated
// for it to its end, or 0 where a cache line cannot keep it: one of more than
// half a line, or one that holds other than instructions whose length is the
// assembler's or a jump's, or with Compacts the linker's to take from only,
// as the load of a pointer for a call is in a section that states spans. A
// loop that ends within it and states its own end is not padded where this
// one is kept.
static unsigned getLoopSize(const MCFragment &F, bool Compacts) {
  const MCSymbol &End = *F.getAlignLoopEnd();
  const MCFragment *I = F.getNext();
  while (I != End.getFragment() && I->getKind() == MCFragment::FT_Align &&
         !I->getFixedSize() && I->getAlignLoopEnd() == &End)
    I = I->getNext();
  auto EndsWithin = [&](const MCSymbol *Inner) {
    if (!Inner->getFragment())
      return false;
    unsigned Order = Inner->getFragment()->getLayoutOrder();
    unsigned EndOrder = End.getFragment()->getLayoutOrder();
    return Order < EndOrder ||
           (Order == EndOrder && Inner->getOffset() <= End.getOffset());
  };
  uint64_t Size = End.getOffset();
  for (; I != End.getFragment(); I = I->getNext()) {
    if (I->getKind() == MCFragment::FT_Align && I->getAlignLoopEnd() &&
        EndsWithin(I->getAlignLoopEnd()))
      Size += I->getFixedSize();
    else if (I->getKind() != MCFragment::FT_Relaxable &&
             (I->getKind() != MCFragment::FT_Data ||
              (I->isLinkerRelaxable() && !Compacts)))
      return 0;
    else
      Size += I->getSize();
  }
  return Size <= 32 ? Size : 0;
}

bool X86AsmBackend::fixupNeedsRelaxationAdvanced(const MCFragment &F,
                                                 const MCFixup &Fixup,
                                                 const MCValue &Target,
                                                 uint64_t Value,
                                                 bool Resolved) const {
  // If resolved, relax if the value is too big for a (signed) i8.
  //
  // Currently, `jmp local@plt` relaxes JMP even if the offset is small,
  // different from gas.
  if (Resolved) {
    // finishLayout folds padding into encodings after relaxation, shifting a
    // branch and its target within their bundles. Keep a bundle of headroom.
    // Immediates do not shift.
    int64_t Slack = Asm->isBundlingEnabled() && TargetPrefixMax != 0 &&
                            isRelaxableBranch(F.getOpcode())
                        ? Asm->getBundleAlign().value()
                        : 0;
    // In a section that states spans, a jump within it is short where it
    // reaches in every link: the linker redoes the padding of an ALIGN
    // relocation where bytes before it went, which may add what the padding
    // lacks of its most to a distance across it. One that reaches in some
    // link is long and the linker's to narrow: between it and its target the
    // linker may take all padding, five bytes at the load of a pointer for a
    // call, and four at such a jump.
    const MCSymbol *To = Target.getAddSym();
    MCSection *Sec = F.getParent();
    bool States =
        Sec->isLinkerRelaxable() && To && To->getFragment() &&
        To->getFragment()->getParent() == Sec &&
        isRelaxableBranch(F.getOpcode()) &&
        static_cast<WinCOFFObjectWriter &>(Asm->getWriter()).statesSpans(*Sec);
    int64_t Least = 0;
    if (States) {
      bool Back = To->getFragment()->getLayoutOrder() <= F.getLayoutOrder();
      const MCFragment *I = Back ? To->getFragment() : F.getNext(),
                       *E = Back ? &F : To->getFragment();
      // The bytes that stay between the two in every link put the target out
      // of reach once they are more than a short jump spans.
      for (int64_t Stay = Back ? -int64_t(To->getOffset()) : To->getOffset();
           I != E && Stay <= 128; I = I->getNext()) {
        int64_t Goes = 0;
        if (I->getKind() == MCFragment::FT_Align) {
          // The padding of a loop kept within a line only goes.
          if (I->isLinkerRelaxable() && !I->getAlignLoopEnd())
            Slack += std::min<uint64_t>(I->getAlignment().value() - 1,
                                        I->getAlignMaxBytesToEmit()) -
                     I->getVarSize();
          if (I->isLinkerRelaxable() || !Back)
            Goes = I->getVarSize();
        } else if (I->getKind() == MCFragment::FT_BoundaryAlign) {
          Goes = I->getSize();
        } else if (I->isLinkerRelaxable()) {
          Goes = I->getKind() == MCFragment::FT_Relaxable ? 4 : 5;
        }
        Least += Goes;
        Stay += int64_t(Asm->computeFragmentSize(*I)) - Goes;
      }
    }
    bool Long = !isInt<8>(int64_t(Value) + Slack) ||
                !isInt<8>(int64_t(Value) - Slack) || Target.getSpecifier();
    if (Long && States && !Target.getSpecifier() &&
        isInt<8>(int64_t(Value) < 0 ? int64_t(Value) + Least
                                    : std::max<int64_t>(Value - Least, 0))) {
      const_cast<MCFragment &>(F).setLinkerRelaxable();
      Sec->setFirstLinkerRelaxable(
          std::min(Sec->firstLinkerRelaxable(), F.getLayoutOrder()));
    }
    return Long;
  }

  // Otherwise, relax unless there is a @ABS8 specifier.
  if (Fixup.getKind() == FK_Data_1 && Target.getAddSym() &&
      Target.getSpecifier() == X86::S_ABS8)
    return false;
  return true;
}

// FIXME: Can tblgen help at all here to verify there aren't other instructions
// we can relax?
void X86AsmBackend::relaxInstruction(MCInst &Inst,
                                     const MCSubtargetInfo &STI) const {
  // The only relaxations X86 does is from a 1byte pcrel to a 4byte pcrel.
  bool Is16BitMode = STI.hasFeature(X86::Is16Bit);
  unsigned RelaxedOp = getRelaxedOpcode(Inst, Is16BitMode);
  assert(RelaxedOp != Inst.getOpcode());
  Inst.setOpcode(RelaxedOp);
}

// States padding that is the linker's to redo: an ALIGN relocation at the
// padding, the variable part of F, packs the alignment, the longest
// no-operation and, from bit 9, the mode into its index.
void X86AsmBackend::stateAlign(MCFragment &F, Align Alignment,
                               unsigned Mode) const {
  F.setVarFixups({MCFixup::create(
      0,
      MCConstantExpr::create(Log2(Alignment) |
                                 getMaximumNopSize(*F.getSubtargetInfo()) << 5 |
                                 Mode << 9,
                             getContext()),
      getCOFFRelocFixupKind(COFF::IMAGE_REL_LLVM_ALIGN))});
  F.setLinkerRelaxable();
}

// Keeps the loop whose alignment F is within a cache line where a line can
// keep it, in a Windows Itanium or NT-POSIX object. Its section stays 16-byte
// aligned and is barred from the places 16 bytes apart within a line at which
// the loop would cross one; the linker places it. Before the first site the
// linker may resize the assembler bars, and pads only where no place is left,
// by the least that leaves one. Behind it the linker bars, by the loops the
// object lists while they leave a place in every link: one of at most 17
// bytes bars one place and a larger one two. Every later loop takes an ALIGN
// in line mode, bit 18 of the mode above the loop's size less one: padding of
// that size, which the linker cuts to the least that leaves a place.
bool X86AsmBackend::keepLoop(MCFragment &F, bool Moves, unsigned &Size) {
  MCSection &Sec = *F.getParent();
  if (!STI.hasFeature(X86::Is64Bit) ||
      !STI.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment())
    return false;
  WinCOFFObjectWriter::LinePlace &P =
      static_cast<WinCOFFObjectWriter &>(Asm->getWriter()).LinePlaces[&Sec];
  // A layout pass comes to the alignments of a section in their order.
  if (F.getLayoutOrder() <= P.Order)
    P = {};
  P.Order = F.getLayoutOrder();
  unsigned Loop = getLoopSize(F, compacts(Sec));
  if (!Loop)
    return false;
  Size = 0;
  // The last alignment of a loop places it, and a loop within one that is
  // kept is kept with it.
  uint64_t At = Asm->getFragmentOffset(F) + F.getFixedSize();
  const MCFragment *Next = F.getNext();
  if ((Next->getKind() == MCFragment::FT_Align && !Next->getFixedSize() &&
       Next->getAlignLoopEnd() == F.getAlignLoopEnd()) ||
      At + Loop <= P.End)
    return true;
  // A section aligned to more than 16 bytes never begins at some places.
  Sec.ensureMinAlignment(Align(16));
  unsigned Never = Sec.getAlign() > Align(32)   ? 14
                   : Sec.getAlign() > Align(16) ? 10
                                                : 0;
  auto Bars = [&](uint64_t Begin) {
    unsigned Places = Never;
    for (unsigned I = 0; I != 4; ++I, Begin += 16)
      Places |= bool((Begin ^ (Begin + Loop - 1)) >> 6) << I;
    return Places;
  };
  unsigned Most = Loop > 17 ? 2 : 1;
  if (!Moves) {
    while ((P.Places | Bars(At + Size)) == 15)
      ++Size;
    P.Places |= Bars(At + Size);
  } else if (popcount(P.Places | Never) + P.Most + Most <= 3) {
    P.Most += Most;
    P.Loops.push_back({At, Loop});
  } else {
    P.Most = 3;
    Size = Loop - 1;
    stateAlign(F, Align(1), 1 << 18 | (Loop - 1) << 13);
  }
  P.End = At + Size + Loop;
  return true;
}

// In a section the linker resizes, alignment after the first instruction it
// may resize is the linker's to redo, but one of a byte, which pads nothing.
// The mode is the most bytes to skip, 0 for any number.
bool X86AsmBackend::relaxAlign(MCFragment &F, unsigned &Size) {
  bool Moves = F.getLayoutOrder() > F.getParent()->firstLinkerRelaxable();
  // A layout pass decides a loop's padding anew, as its own ALIGN or none.
  if (F.getAlignLoopEnd()) {
    F.clearVarFixups();
    if (keepLoop(F, Moves, Size))
      return true;
    F.setAlignLoopEnd(nullptr);
  }
  if (!Moves || F.getAlignment() == Align(1))
    return false;
  unsigned MaxSkip = F.getAlignMaxBytesToEmit() < F.getAlignment().value()
                         ? F.getAlignMaxBytesToEmit()
                         : 0;
  if (!isUInt<12>(MaxSkip))
    return false;
  if (Size > F.getAlignMaxBytesToEmit())
    Size = 0;
  stateAlign(F, F.getAlignment(), MaxSkip);
  return true;
}

bool X86AsmBackend::padInstructionViaPrefix(MCFragment &RF,
                                            MCCodeEmitter &Emitter,
                                            unsigned &RemainingSize) const {
  if (!RF.getAllowAutoPadding())
    return false;
  // If the instruction isn't fully relaxed, shifting it around might require a
  // larger value for one of the fixups then can be encoded.  The outer loop
  // will also catch this before moving to the next instruction, but we need to
  // prevent padding this single instruction as well.
  if (mayNeedRelaxation(RF.getOpcode(), RF.getOperands(),
                        *RF.getSubtargetInfo()))
    return false;

  const unsigned OldSize = RF.getVarSize();
  if (OldSize == 15)
    return false;

  const unsigned MaxPossiblePad = std::min(15 - OldSize, RemainingSize);
  const unsigned RemainingPrefixSize = [&]() -> unsigned {
    SmallString<15> Code;
    X86_MC::emitPrefix(Emitter, RF.getInst(), Code, STI);
    assert(Code.size() < 15 && "The number of prefixes must be less than 15.");

    // TODO: It turns out we need a decent amount of plumbing for the target
    // specific bits to determine number of prefixes its safe to add.  Various
    // targets (older chips mostly, but also Atom family) encounter decoder
    // stalls with too many prefixes.  For testing purposes, we set the value
    // externally for the moment.
    unsigned ExistingPrefixSize = Code.size();
    if (TargetPrefixMax <= ExistingPrefixSize)
      return 0;
    return TargetPrefixMax - ExistingPrefixSize;
  }();
  const unsigned PrefixBytesToAdd =
      std::min(MaxPossiblePad, RemainingPrefixSize);
  if (PrefixBytesToAdd == 0)
    return false;

  const uint8_t Prefix =
      X86_MC::determinePaddingPrefix(RF.getInst(), *MCII, STI);

  SmallString<256> Code;
  Code.append(PrefixBytesToAdd, Prefix);
  Code.append(RF.getVarContents().begin(), RF.getVarContents().end());
  RF.setVarContents(Code);

  // Adjust the fixups for the change in offsets
  for (auto &F : RF.getVarFixups())
    F.setOffset(PrefixBytesToAdd + F.getOffset());

  RemainingSize -= PrefixBytesToAdd;
  return true;
}

bool X86AsmBackend::padInstructionViaRelaxation(MCFragment &RF,
                                                MCCodeEmitter &Emitter,
                                                unsigned &RemainingSize) const {
  if (!mayNeedRelaxation(RF.getOpcode(), RF.getOperands(),
                         *RF.getSubtargetInfo()))
    // TODO: There are lots of other tricks we could apply for increasing
    // encoding size without impacting performance.
    return false;

  MCInst Relaxed = RF.getInst();
  relaxInstruction(Relaxed, *RF.getSubtargetInfo());

  SmallVector<MCFixup, 4> Fixups;
  SmallString<15> Code;
  Emitter.encodeInstruction(Relaxed, Code, Fixups, *RF.getSubtargetInfo());
  const unsigned OldSize = RF.getVarContents().size();
  const unsigned NewSize = Code.size();
  assert(NewSize >= OldSize && "size decrease during relaxation?");
  unsigned Delta = NewSize - OldSize;
  if (Delta > RemainingSize)
    return false;
  RF.setInst(Relaxed);
  RF.setVarContents(Code);
  RF.setVarFixups(Fixups);
  RemainingSize -= Delta;
  return true;
}

bool X86AsmBackend::padInstructionEncoding(MCFragment &RF,
                                           MCCodeEmitter &Emitter,
                                           unsigned &RemainingSize) const {
  bool Changed = false;
  if (RemainingSize != 0)
    Changed |= padInstructionViaRelaxation(RF, Emitter, RemainingSize);
  if (RemainingSize != 0)
    Changed |= padInstructionViaPrefix(RF, Emitter, RemainingSize);
  return Changed;
}

bool X86AsmBackend::padInstsBackward(SmallVectorImpl<MCFragment *> &Relaxable,
                                     unsigned &RemainingSize) const {
  bool Changed = false;
  while (!Relaxable.empty() && RemainingSize != 0) {
    auto &RF = *Relaxable.pop_back_val();
    // Give the backend a chance to play any tricks it wishes to increase
    // the encoding size of the given instruction.  Target independent code
    // will try further relaxation, but target's may play further tricks.
    Changed |= padInstructionEncoding(RF, Asm->getEmitter(), RemainingSize);

    // If we have an instruction which hasn't been fully relaxed, we can't
    // skip past it and insert bytes before it.  Changing its starting
    // offset might require a larger negative offset than it can encode.
    // We don't need to worry about larger positive offsets as none of the
    // possible offsets between this and our align are visible, and the
    // ones afterwards aren't changing.
    if (mayNeedRelaxation(RF.getOpcode(), RF.getOperands(),
                          *RF.getSubtargetInfo()))
      break;
  }
  Relaxable.clear();
  return Changed;
}

/// Trade the padding held by \p BF for ignored prefixes on the instructions
/// around it. Padding never leaves its own bundle, so no instruction or
/// bundle-locked group moves across a boundary. \p Relaxable holds the
/// preceding instructions in that bundle and is consumed.
bool X86AsmBackend::foldBundlePad(
    const MCAssembler &Asm, MCBoundaryAlignFragment &BF,
    SmallVectorImpl<MCFragment *> &Relaxable) const {
  const uint64_t BundleSize = Asm.getBundleAlign().value();
  const uint64_t PadStart = Asm.getFragmentOffset(BF);
  unsigned Remaining = BF.getSize();

  // Only padding in PadStart's own bundle may move backward; an align_to_end
  // group can push the rest into the next bundle.
  unsigned Budget =
      std::min<uint64_t>(Remaining, BundleSize - PadStart % BundleSize);
  unsigned Left = Budget;
  bool Changed = padInstsBackward(Relaxable, Left);
  Remaining -= Budget - Left;

  // Absorbing padding moves the group's start earlier while its end is pinned,
  // so it may only grow into the slack in its bundle: BundleSize - GroupSize
  // for align_to_end, zero for a group that already starts on a boundary.
  if (BF.isAlignToEnd() && Remaining) {
    uint64_t GroupSize = 0;
    for (const MCFragment *F = BF.getNext();; F = F->getNext()) {
      GroupSize += Asm.computeFragmentSize(*F);
      if (F == BF.getLastFragment())
        break;
    }
    if (GroupSize < BundleSize) {
      Left = Budget = std::min<uint64_t>(Remaining, BundleSize - GroupSize);
      for (MCFragment *F = BF.getNext(); F; F = F->getNext()) {
        if (F->getKind() == MCFragment::FT_Relaxable)
          Changed |= padInstructionEncoding(*F, Asm.getEmitter(), Left);
        if (F == BF.getLastFragment() || Left == 0)
          break;
      }
      Remaining -= Budget - Left;
    }
  }

  BF.setSize(Remaining);
  return Changed;
}

bool X86AsmBackend::optimizeBundleNops(const MCAssembler &Asm) const {
  const uint64_t BundleSize = Asm.getBundleAlign().value();
  bool Changed = false;
  for (MCSection &Sec : Asm) {
    if (!Sec.isText())
      continue;

    // Instructions preceding the next padding and sharing its bundle.
    SmallVector<MCFragment *, 8> Relaxable;
    // Folding leaves stale offsets until the next layout, so skip the
    // rewritten range.
    const MCFragment *ResumeAfter = nullptr;
    for (MCFragment &F : Sec) {
      if (ResumeAfter) {
        if (&F == ResumeAfter)
          ResumeAfter = nullptr;
        continue;
      }
      uint64_t Offset = Asm.getFragmentOffset(F);
      if (!Relaxable.empty() &&
          Asm.getFragmentOffset(*Relaxable.front()) / BundleSize !=
              Offset / BundleSize)
        Relaxable.clear();

      switch (F.getKind()) {
      case MCFragment::FT_BoundaryAlign: {
        auto &BF = static_cast<MCBoundaryAlignFragment &>(F);
        if (!BF.getSize())
          break; // Nothing to fold, and not a barrier.
        Changed |= foldBundlePad(Asm, BF, Relaxable);
        ResumeAfter = BF.getLastFragment();
        break;
      }
      case MCFragment::FT_Relaxable:
        Relaxable.push_back(&F);
        break;
      case MCFragment::FT_Data:
        break; // Fixed bytes, safe to shift.
      default:
        // Other kinds may change size when shifted (.p2align, .org, LEBs).
        Relaxable.clear();
        break;
      }
    }
  }

  return Changed;
}

bool X86AsmBackend::finishLayout() const {
  // With bundling, padding is fully determined during layout and the only
  // post-layout optimization is prefix padding.
  if (Asm->isBundlingEnabled())
    return TargetPrefixMax != 0 && optimizeBundleNops(*Asm);

  // In a section the linker resizes, padding that keeps a branch within a
  // boundary is the linker's to redo. The mode is bit 12 and, above it, the
  // length of the instructions that follow the padding, which fits 5 bits.
  for (MCSection &Sec : *Asm) {
    if (!Sec.isLinkerRelaxable())
      continue;
    for (MCFragment &F : Sec) {
      auto *BF = dyn_cast<MCBoundaryAlignFragment>(&F);
      if (!BF || !BF->getLastFragment())
        continue;
      unsigned Span = 0;
      for (const MCFragment *I = BF; I != BF->getLastFragment();) {
        I = I->getNext();
        Span = I->getKind() == MCFragment::FT_Data ||
                       I->getKind() == MCFragment::FT_Relaxable
                   ? Span + I->getSize()
                   : 32;
      }
      if (isUInt<5>(Span))
        stateAlign(F, BF->getAlignment(), 1 << 12 | Span << 13);
    }
  }
  // See if we can further relax some instructions to cut down on the number of
  // nop bytes required for code alignment.  The actual win is in reducing
  // instruction count, not number of bytes.  Modern X86-64 can easily end up
  // decode limited.  It is often better to reduce the number of instructions
  // (i.e. eliminate nops) even at the cost of increasing the size and
  // complexity of others.
  if (!CLOpts.pad_for_align && !CLOpts.pad_for_branch_align)
    return false;

  // The processed regions are delimitered by LabeledFragments. -g may have more
  // MCSymbols and therefore different relaxation results. -x86-pad-for-align is
  // disabled by default to eliminate the -g vs non -g difference.
  DenseSet<MCFragment *> LabeledFragments;
  for (const MCSymbol &S : Asm->symbols())
    LabeledFragments.insert(S.getFragment());

  bool Changed = false;
  for (MCSection &Sec : *Asm) {
    if (!Sec.isText())
      continue;

    SmallVector<MCFragment *, 4> Relaxable;
    for (MCSection::iterator I = Sec.begin(), IE = Sec.end(); I != IE; ++I) {
      MCFragment &F = *I;

      if (LabeledFragments.count(&F))
        Relaxable.clear();

      if (F.getKind() == MCFragment::FT_Data) // Skip and ignore
        continue;

      if (F.getKind() == MCFragment::FT_Relaxable) {
        auto &RF = cast<MCFragment>(*I);
        Relaxable.push_back(&RF);
        continue;
      }

      auto canHandle = [&](MCFragment &F) -> bool {
        switch (F.getKind()) {
        default:
          return false;
        case MCFragment::FT_Align:
          return CLOpts.pad_for_align;
        case MCFragment::FT_BoundaryAlign:
          return CLOpts.pad_for_branch_align || Sec.isLinkerRelaxable();
        }
      };
      // For any unhandled kind, assume we can't change layout.
      if (!canHandle(F)) {
        Relaxable.clear();
        continue;
      }

      // To keep the effects local, prefer to relax instructions closest to
      // the align directive.  This is purely about human understandability
      // of the resulting code.  If we later find a reason to expand
      // particular instructions over others, we can adjust.
      // The linker redoes such padding as no-operations only, so none of it
      // becomes prefixes.
      unsigned RemainingSize = Asm->computeFragmentSize(F) - F.getFixedSize();
      if (F.getKind() == MCFragment::FT_BoundaryAlign && Sec.isLinkerRelaxable())
        Relaxable.clear();
      else
        Changed |= padInstsBackward(Relaxable, RemainingSize);

      // If we're looking at a boundary align, make sure we don't try to pad
      // its target instructions for some following directive.  Doing so would
      // break the alignment of the current boundary align.
      if (auto *BF = dyn_cast<MCBoundaryAlignFragment>(&F)) {
        cast<MCBoundaryAlignFragment>(F).setSize(RemainingSize);
        Changed = true;
        const MCFragment *LastFragment = BF->getLastFragment();
        if (!LastFragment)
          continue;
        while (&*I != LastFragment)
          ++I;
      }
    }
  }

  return Changed;
}

unsigned X86AsmBackend::getMaximumNopSize(const MCSubtargetInfo &STI) const {
  if (STI.hasFeature(X86::Is16Bit))
    return 4;
  if (!STI.hasFeature(X86::FeatureNOPL) && !STI.hasFeature(X86::Is64Bit))
    return 1;
  if (STI.hasFeature(X86::TuningFast7ByteNOP))
    return 7;
  if (STI.hasFeature(X86::TuningFast15ByteNOP))
    return 15;
  if (STI.hasFeature(X86::TuningFast11ByteNOP))
    return 11;
  // FIXME: handle 32-bit mode
  // 15-bytes is the longest single NOP instruction, but 10-bytes is
  // commonly the longest that can be efficiently decoded.
  return 10;
}

/// Write a sequence of optimal nops to the output, covering \p Count
/// bytes.
/// \return - true on success, false on failure
bool X86AsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                 const MCSubtargetInfo *STI) const {
  static const char Nops32Bit[10][11] = {
      // nop
      "\x90",
      // xchg %ax,%ax
      "\x66\x90",
      // nopl (%[re]ax)
      "\x0f\x1f\x00",
      // nopl 0(%[re]ax)
      "\x0f\x1f\x40\x00",
      // nopl 0(%[re]ax,%[re]ax,1)
      "\x0f\x1f\x44\x00\x00",
      // nopw 0(%[re]ax,%[re]ax,1)
      "\x66\x0f\x1f\x44\x00\x00",
      // nopl 0L(%[re]ax)
      "\x0f\x1f\x80\x00\x00\x00\x00",
      // nopl 0L(%[re]ax,%[re]ax,1)
      "\x0f\x1f\x84\x00\x00\x00\x00\x00",
      // nopw 0L(%[re]ax,%[re]ax,1)
      "\x66\x0f\x1f\x84\x00\x00\x00\x00\x00",
      // nopw %cs:0L(%[re]ax,%[re]ax,1)
      "\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00",
  };

  // 16-bit mode uses different nop patterns than 32-bit.
  static const char Nops16Bit[4][11] = {
      // nop
      "\x90",
      // xchg %eax,%eax
      "\x66\x90",
      // lea 0(%si),%si
      "\x8d\x74\x00",
      // lea 0w(%si),%si
      "\x8d\xb4\x00\x00",
  };

  const char(*Nops)[11] =
      STI->hasFeature(X86::Is16Bit) ? Nops16Bit : Nops32Bit;

  uint64_t MaxNopLength = (uint64_t)getMaximumNopSize(*STI);

  // Emit as many MaxNopLength NOPs as needed, then emit a NOP of the remaining
  // length.
  do {
    const uint8_t ThisNopLength = (uint8_t) std::min(Count, MaxNopLength);
    const uint8_t Prefixes = ThisNopLength <= 10 ? 0 : ThisNopLength - 10;
    for (uint8_t i = 0; i < Prefixes; i++)
      OS << '\x66';
    const uint8_t Rest = ThisNopLength - Prefixes;
    if (Rest != 0)
      OS.write(Nops[Rest - 1], Rest);
    Count -= ThisNopLength;
  } while (Count != 0);

  return true;
}

/* *** */

namespace {

class ELFX86AsmBackend : public X86AsmBackend {
public:
  uint8_t OSABI;
  ELFX86AsmBackend(const X86MCOptions &CLOpts, const Target &T, uint8_t OSABI,
                   const MCSubtargetInfo &STI)
      : X86AsmBackend(CLOpts, T, STI), OSABI(OSABI) {}
};

class ELFX86_32AsmBackend : public ELFX86AsmBackend {
public:
  using ELFX86AsmBackend::ELFX86AsmBackend;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createX86ELFObjectWriter(/*IsELF64*/ false, OSABI, ELF::EM_386);
  }
};

class ELFX86_X32AsmBackend : public ELFX86AsmBackend {
public:
  using ELFX86AsmBackend::ELFX86AsmBackend;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createX86ELFObjectWriter(/*IsELF64*/ false, OSABI,
                                    ELF::EM_X86_64);
  }
};

class ELFX86_IAMCUAsmBackend : public ELFX86AsmBackend {
public:
  using ELFX86AsmBackend::ELFX86AsmBackend;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createX86ELFObjectWriter(/*IsELF64*/ false, OSABI,
                                    ELF::EM_IAMCU);
  }
};

class ELFX86_64AsmBackend : public ELFX86AsmBackend {
public:
  using ELFX86AsmBackend::ELFX86AsmBackend;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createX86ELFObjectWriter(/*IsELF64*/ true, OSABI, ELF::EM_X86_64);
  }
};

class WindowsX86AsmBackend : public X86AsmBackend {
  bool Is64Bit;

public:
  WindowsX86AsmBackend(const X86MCOptions &CLOpts, const Target &T,
                       bool is64Bit, const MCSubtargetInfo &STI)
      : X86AsmBackend(CLOpts, T, STI), Is64Bit(is64Bit) {}

  std::optional<MCFixupKind> getFixupKind(StringRef Name) const override {
    // On x86-64 .reloc also states the types that mark a place and encode no
    // field.
    unsigned Type =
        StringSwitch<unsigned>(Name)
            .Case("IMAGE_REL_AMD64_ABSOLUTE", COFF::IMAGE_REL_AMD64_ABSOLUTE)
            .Case("IMAGE_REL_AMD64_SREL32", COFF::IMAGE_REL_AMD64_SREL32)
            .Case("IMAGE_REL_LLVM_ALIGN", COFF::IMAGE_REL_LLVM_ALIGN)
            .Default(-1u);
    // A Windows Itanium or NT-POSIX object marks its fused sites, and the
    // sequence that forms an image's thread-local base.
    if (getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
      if (Name == "IMAGE_REL_LLVM_TLS_SCALE")
        Type = COFF::IMAGE_REL_LLVM_TLS_SCALE;
      else if (const COFF::FusedMarker *Marker = COFF::findFusedMarker(
                   COFF::IMAGE_FILE_MACHINE_AMD64,
                   [&](const COFF::FusedMarker &M) { return M.Name == Name; }))
        Type = Marker->Type;
    }
    if (Is64Bit && Type != -1u)
      return getCOFFRelocFixupKind(Type);
    return StringSwitch<std::optional<MCFixupKind>>(Name)
        .Case("dir32", FK_Data_4)
        .Case("secrel32", FK_SecRel_4)
        .Case("secidx", FK_SecRel_2)
        .Default(MCAsmBackend::getFixupKind(Name));
  }

  // On Windows Itanium and NT-POSIX a label difference the linker may change
  // is a fixup, which the object writer records as a span.
  bool relaxDwarfLineAddr(MCFragment &F) const override {
    return getContext()
               .getTargetTriple()
               .isWindowsItaniumOrNTPOSIXEnvironment() &&
           relaxDwarfLineAddrWithFixup(F);
  }
  bool relaxDwarfCFA(MCFragment &F) const override {
    return getContext()
               .getTargetTriple()
               .isWindowsItaniumOrNTPOSIXEnvironment() &&
           relaxDwarfCFAWithFixups(
               F, 8,
               [&](unsigned Bits, unsigned Offset,
                   SmallVectorImpl<MCFixup> &Fixups) {
                 Fixups.push_back(
                     MCFixup::create(Offset, &F.getDwarfAddrDelta(),
                                     MCFixup::getDataKindForSize(Bits / 8)));
               });
  }
  std::pair<bool, bool> relaxLEB128(MCFragment &F,
                                    int64_t &Value) const override {
    if (!getContext()
             .getTargetTriple()
             .isWindowsItaniumOrNTPOSIXEnvironment() ||
        !F.getLEBValue().evaluateKnownAbsolute(Value, *Asm))
      return {false, false};
    F.setVarFixups({MCFixup::create(0, &F.getLEBValue(), FK_Data_leb128)});
    return {true, false};
  }

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createX86WinCOFFObjectWriter(Is64Bit);
  }
};

namespace CU {

  /// Compact unwind encoding values.
  enum CompactUnwindEncodings {
    /// [RE]BP based frame where [RE]BP is pused on the stack immediately after
    /// the return address, then [RE]SP is moved to [RE]BP.
    UNWIND_MODE_BP_FRAME                   = 0x01000000,

    /// A frameless function with a small constant stack size.
    UNWIND_MODE_STACK_IMMD                 = 0x02000000,

    /// A frameless function with a large constant stack size.
    UNWIND_MODE_STACK_IND                  = 0x03000000,

    /// No compact unwind encoding is available.
    UNWIND_MODE_DWARF                      = 0x04000000,

    /// Mask for encoding the frame registers.
    UNWIND_BP_FRAME_REGISTERS              = 0x00007FFF,

    /// Mask for encoding the frameless registers.
    UNWIND_FRAMELESS_STACK_REG_PERMUTATION = 0x000003FF
  };

} // namespace CU

class DarwinX86AsmBackend : public X86AsmBackend {
  const MCRegisterInfo &MRI;

  /// Number of registers that can be saved in a compact unwind encoding.
  enum { CU_NUM_SAVED_REGS = 6 };

  mutable unsigned SavedRegs[CU_NUM_SAVED_REGS];
  Triple TT;
  bool Is64Bit;

  unsigned OffsetSize;                   ///< Offset of a "push" instruction.
  unsigned MoveInstrSize;                ///< Size of a "move" instruction.
  unsigned StackDivide;                  ///< Amount to adjust stack size by.
protected:
  /// Size of a "push" instruction for the given register.
  unsigned PushInstrSize(MCRegister Reg) const {
    switch (Reg.id()) {
      case X86::EBX:
      case X86::ECX:
      case X86::EDX:
      case X86::EDI:
      case X86::ESI:
      case X86::EBP:
      case X86::RBX:
      case X86::RBP:
        return 1;
      case X86::R12:
      case X86::R13:
      case X86::R14:
      case X86::R15:
        return 2;
    }
    return 1;
  }

private:
  /// Get the compact unwind number for a given register. The number
  /// corresponds to the enum lists in compact_unwind_encoding.h.
  int getCompactUnwindRegNum(unsigned Reg) const {
    static const MCPhysReg CU32BitRegs[7] = {
      X86::EBX, X86::ECX, X86::EDX, X86::EDI, X86::ESI, X86::EBP, 0
    };
    static const MCPhysReg CU64BitRegs[] = {
      X86::RBX, X86::R12, X86::R13, X86::R14, X86::R15, X86::RBP, 0
    };
    const MCPhysReg *CURegs = Is64Bit ? CU64BitRegs : CU32BitRegs;
    for (int Idx = 1; *CURegs; ++CURegs, ++Idx)
      if (*CURegs == Reg)
        return Idx;

    return -1;
  }

  /// Return the registers encoded for a compact encoding with a frame
  /// pointer.
  uint32_t encodeCompactUnwindRegistersWithFrame() const {
    // Encode the registers in the order they were saved --- 3-bits per
    // register. The list of saved registers is assumed to be in reverse
    // order. The registers are numbered from 1 to CU_NUM_SAVED_REGS.
    uint32_t RegEnc = 0;
    for (int i = 0, Idx = 0; i != CU_NUM_SAVED_REGS; ++i) {
      unsigned Reg = SavedRegs[i];
      if (Reg == 0) break;

      int CURegNum = getCompactUnwindRegNum(Reg);
      if (CURegNum == -1) return ~0U;

      // Encode the 3-bit register number in order, skipping over 3-bits for
      // each register.
      RegEnc |= (CURegNum & 0x7) << (Idx++ * 3);
    }

    assert((RegEnc & 0x3FFFF) == RegEnc &&
           "Invalid compact register encoding!");
    return RegEnc;
  }

  /// Create the permutation encoding used with frameless stacks. It is
  /// passed the number of registers to be saved and an array of the registers
  /// saved.
  uint32_t encodeCompactUnwindRegistersWithoutFrame(unsigned RegCount) const {
    // The saved registers are numbered from 1 to 6. In order to encode the
    // order in which they were saved, we re-number them according to their
    // place in the register order. The re-numbering is relative to the last
    // re-numbered register. E.g., if we have registers {6, 2, 4, 5} saved in
    // that order:
    //
    //    Orig  Re-Num
    //    ----  ------
    //     6       6
    //     2       2
    //     4       3
    //     5       3
    //
    for (unsigned i = 0; i < RegCount; ++i) {
      int CUReg = getCompactUnwindRegNum(SavedRegs[i]);
      if (CUReg == -1) return ~0U;
      SavedRegs[i] = CUReg;
    }

    // Reverse the list.
    std::reverse(&SavedRegs[0], &SavedRegs[CU_NUM_SAVED_REGS]);

    uint32_t RenumRegs[CU_NUM_SAVED_REGS];
    for (unsigned i = CU_NUM_SAVED_REGS - RegCount; i < CU_NUM_SAVED_REGS; ++i){
      unsigned Countless = 0;
      for (unsigned j = CU_NUM_SAVED_REGS - RegCount; j < i; ++j)
        if (SavedRegs[j] < SavedRegs[i])
          ++Countless;

      RenumRegs[i] = SavedRegs[i] - Countless - 1;
    }

    // Take the renumbered values and encode them into a 10-bit number.
    uint32_t permutationEncoding = 0;
    switch (RegCount) {
    case 6:
      permutationEncoding |= 120 * RenumRegs[0] + 24 * RenumRegs[1]
                             + 6 * RenumRegs[2] +  2 * RenumRegs[3]
                             +     RenumRegs[4];
      break;
    case 5:
      permutationEncoding |= 120 * RenumRegs[1] + 24 * RenumRegs[2]
                             + 6 * RenumRegs[3] +  2 * RenumRegs[4]
                             +     RenumRegs[5];
      break;
    case 4:
      permutationEncoding |=  60 * RenumRegs[2] + 12 * RenumRegs[3]
                             + 3 * RenumRegs[4] +      RenumRegs[5];
      break;
    case 3:
      permutationEncoding |=  20 * RenumRegs[3] +  4 * RenumRegs[4]
                             +     RenumRegs[5];
      break;
    case 2:
      permutationEncoding |=   5 * RenumRegs[4] +      RenumRegs[5];
      break;
    case 1:
      permutationEncoding |=       RenumRegs[5];
      break;
    }

    assert((permutationEncoding & 0x3FF) == permutationEncoding &&
           "Invalid compact register encoding!");
    return permutationEncoding;
  }

public:
  DarwinX86AsmBackend(const X86MCOptions &CLOpts, const Target &T,
                      const MCRegisterInfo &MRI, const MCSubtargetInfo &STI)
      : X86AsmBackend(CLOpts, T, STI), MRI(MRI), TT(STI.getTargetTriple()),
        Is64Bit(TT.isX86_64()) {
    memset(SavedRegs, 0, sizeof(SavedRegs));
    OffsetSize = Is64Bit ? 8 : 4;
    MoveInstrSize = Is64Bit ? 3 : 2;
    StackDivide = Is64Bit ? 8 : 4;
  }

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    uint32_t CPUType = cantFail(MachO::getCPUType(TT));
    uint32_t CPUSubType = cantFail(MachO::getCPUSubType(TT));
    return createX86MachObjectWriter(Is64Bit, CPUType, CPUSubType);
  }

  /// Implementation of algorithm to generate the compact unwind encoding
  /// for the CFI instructions.
  uint64_t generateCompactUnwindEncoding(const MCDwarfFrameInfo *FI,
                                         const MCContext *Ctxt) const override {
    if (Ctxt->emitDwarfUnwindInfo() == EmitDwarfUnwindType::DwarfOnly)
      return CU::UNWIND_MODE_DWARF;

    // Signal frames cannot be encoded in compact unwind.
    if (FI->IsSignalFrame)
      return CU::UNWIND_MODE_DWARF;

    ArrayRef<MCCFIInstruction> Instrs = FI->Instructions;
    if (Instrs.empty()) return 0;
    if (!isDarwinCanonicalPersonality(FI->Personality) &&
        !Ctxt->emitCompactUnwindNonCanonical())
      return CU::UNWIND_MODE_DWARF;

    // Reset the saved registers.
    unsigned SavedRegIdx = 0;
    memset(SavedRegs, 0, sizeof(SavedRegs));

    bool HasFP = false;

    // Encode that we are using EBP/RBP as the frame pointer.
    uint64_t CompactUnwindEncoding = 0;

    unsigned SubtractInstrIdx = Is64Bit ? 3 : 2;
    unsigned InstrOffset = 0;
    unsigned StackAdjust = 0;
    uint64_t StackSize = 0;
    int64_t MinAbsOffset = std::numeric_limits<int64_t>::max();

    for (const MCCFIInstruction &Inst : Instrs) {
      switch (Inst.getOperation()) {
      default:
        // Any other CFI directives indicate a frame that we aren't prepared
        // to represent via compact unwind, so just bail out.
        return CU::UNWIND_MODE_DWARF;
      case MCCFIInstruction::OpDefCfaRegister: {
        // Defines a frame pointer. E.g.
        //
        //     movq %rsp, %rbp
        //  L0:
        //     .cfi_def_cfa_register %rbp
        //
        HasFP = true;

        // If the frame pointer is other than esp/rsp, we do not have a way to
        // generate a compact unwinding representation, so bail out.
        if (*MRI.getLLVMRegNum(Inst.getRegister(), true) !=
            (Is64Bit ? X86::RBP : X86::EBP))
          return CU::UNWIND_MODE_DWARF;

        // Reset the counts.
        memset(SavedRegs, 0, sizeof(SavedRegs));
        StackAdjust = 0;
        SavedRegIdx = 0;
        MinAbsOffset = std::numeric_limits<int64_t>::max();
        InstrOffset += MoveInstrSize;
        break;
      }
      case MCCFIInstruction::OpDefCfaOffset: {
        // Defines a new offset for the CFA. E.g.
        //
        //  With frame:
        //
        //     pushq %rbp
        //  L0:
        //     .cfi_def_cfa_offset 16
        //
        //  Without frame:
        //
        //     subq $72, %rsp
        //  L0:
        //     .cfi_def_cfa_offset 80
        //
        StackSize = Inst.getOffset() / StackDivide;
        break;
      }
      case MCCFIInstruction::OpOffset: {
        // Defines a "push" of a callee-saved register. E.g.
        //
        //     pushq %r15
        //     pushq %r14
        //     pushq %rbx
        //  L0:
        //     subq $120, %rsp
        //  L1:
        //     .cfi_offset %rbx, -40
        //     .cfi_offset %r14, -32
        //     .cfi_offset %r15, -24
        //
        if (SavedRegIdx == CU_NUM_SAVED_REGS)
          // If there are too many saved registers, we cannot use a compact
          // unwind encoding.
          return CU::UNWIND_MODE_DWARF;

        MCRegister Reg = *MRI.getLLVMRegNum(Inst.getRegister(), true);
        SavedRegs[SavedRegIdx++] = Reg.id();
        StackAdjust += OffsetSize;
        MinAbsOffset = std::min(MinAbsOffset, std::abs(Inst.getOffset()));
        InstrOffset += PushInstrSize(Reg);
        break;
      }
      }
    }

    StackAdjust /= StackDivide;

    if (HasFP) {
      if ((StackAdjust & 0xFF) != StackAdjust)
        // Offset was too big for a compact unwind encoding.
        return CU::UNWIND_MODE_DWARF;

      // We don't attempt to track a real StackAdjust, so if the saved registers
      // aren't adjacent to rbp we can't cope.
      if (SavedRegIdx != 0 && MinAbsOffset != 3 * (int)OffsetSize)
        return CU::UNWIND_MODE_DWARF;

      // Get the encoding of the saved registers when we have a frame pointer.
      uint32_t RegEnc = encodeCompactUnwindRegistersWithFrame();
      if (RegEnc == ~0U) return CU::UNWIND_MODE_DWARF;

      CompactUnwindEncoding |= CU::UNWIND_MODE_BP_FRAME;
      CompactUnwindEncoding |= (StackAdjust & 0xFF) << 16;
      CompactUnwindEncoding |= RegEnc & CU::UNWIND_BP_FRAME_REGISTERS;
    } else {
      SubtractInstrIdx += InstrOffset;
      ++StackAdjust;

      if ((StackSize & 0xFF) == StackSize) {
        // Frameless stack with a small stack size.
        CompactUnwindEncoding |= CU::UNWIND_MODE_STACK_IMMD;

        // Encode the stack size.
        CompactUnwindEncoding |= (StackSize & 0xFF) << 16;
      } else {
        if ((StackAdjust & 0x7) != StackAdjust)
          // The extra stack adjustments are too big for us to handle.
          return CU::UNWIND_MODE_DWARF;

        // Frameless stack with an offset too large for us to encode compactly.
        CompactUnwindEncoding |= CU::UNWIND_MODE_STACK_IND;

        // Encode the offset to the nnnnnn value in the 'subl $nnnnnn, ESP'
        // instruction.
        CompactUnwindEncoding |= (SubtractInstrIdx & 0xFF) << 16;

        // Encode any extra stack adjustments (done via push instructions).
        CompactUnwindEncoding |= (StackAdjust & 0x7) << 13;
      }

      // Encode the number of registers saved. (Reverse the list first.)
      std::reverse(&SavedRegs[0], &SavedRegs[SavedRegIdx]);
      CompactUnwindEncoding |= (SavedRegIdx & 0x7) << 10;

      // Get the encoding of the saved registers when we don't have a frame
      // pointer.
      uint32_t RegEnc = encodeCompactUnwindRegistersWithoutFrame(SavedRegIdx);
      if (RegEnc == ~0U) return CU::UNWIND_MODE_DWARF;

      // Encode the register encoding.
      CompactUnwindEncoding |=
        RegEnc & CU::UNWIND_FRAMELESS_STACK_REG_PERMUTATION;
    }

    return CompactUnwindEncoding;
  }
};

} // end anonymous namespace

MCAsmBackend *llvm::createX86_32AsmBackend(const Target &T,
                                           const MCSubtargetInfo &STI,
                                           const MCRegisterInfo &MRI,
                                           const MCTargetOptions &Options) {
  const Triple &TheTriple = STI.getTargetTriple();
  const X86MCOptions &CLOpts = X86_MC::getCLOpts(Options);
  if (TheTriple.isOSBinFormatMachO())
    return new DarwinX86AsmBackend(CLOpts, T, MRI, STI);

  if (TheTriple.isOSWindows() && TheTriple.isOSBinFormatCOFF())
    return new WindowsX86AsmBackend(CLOpts, T, false, STI);

  uint8_t OSABI = MCELFObjectTargetWriter::getOSABI(TheTriple.getOS());

  if (TheTriple.isOSIAMCU())
    return new ELFX86_IAMCUAsmBackend(CLOpts, T, OSABI, STI);

  return new ELFX86_32AsmBackend(CLOpts, T, OSABI, STI);
}

MCAsmBackend *llvm::createX86_64AsmBackend(const Target &T,
                                           const MCSubtargetInfo &STI,
                                           const MCRegisterInfo &MRI,
                                           const MCTargetOptions &Options) {
  const Triple &TheTriple = STI.getTargetTriple();
  const X86MCOptions &CLOpts = X86_MC::getCLOpts(Options);
  if (TheTriple.isOSBinFormatMachO())
    return new DarwinX86AsmBackend(CLOpts, T, MRI, STI);

  if (TheTriple.isOSWindows() && TheTriple.isOSBinFormatCOFF())
    return new WindowsX86AsmBackend(CLOpts, T, true, STI);

  if (TheTriple.isUEFI()) {
    assert(TheTriple.isOSBinFormatCOFF() &&
         "Only COFF format is supported in UEFI environment.");
    return new WindowsX86AsmBackend(CLOpts, T, true, STI);
  }

  uint8_t OSABI = MCELFObjectTargetWriter::getOSABI(TheTriple.getOS());

  if (TheTriple.isX32())
    return new ELFX86_X32AsmBackend(CLOpts, T, OSABI, STI);
  return new ELFX86_64AsmBackend(CLOpts, T, OSABI, STI);
}

namespace {
class X86ELFStreamer : public MCELFStreamer {
public:
  X86ELFStreamer(MCContext &Context, std::unique_ptr<MCAsmBackend> TAB,
                 std::unique_ptr<MCObjectWriter> OW,
                 std::unique_ptr<MCCodeEmitter> Emitter)
      : MCELFStreamer(Context, std::move(TAB), std::move(OW),
                      std::move(Emitter)) {}

  void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override;
};
} // end anonymous namespace

void X86ELFStreamer::emitInstruction(const MCInst &Inst,
                                     const MCSubtargetInfo &STI) {
  X86_MC::emitInstruction(*this, Inst, STI);
}

MCStreamer *llvm::createX86ELFStreamer(const Triple &T, MCContext &Context,
                                       std::unique_ptr<MCAsmBackend> &&MAB,
                                       std::unique_ptr<MCObjectWriter> &&MOW,
                                       std::unique_ptr<MCCodeEmitter> &&MCE) {
  return new X86ELFStreamer(Context, std::move(MAB), std::move(MOW),
                            std::move(MCE));
}
