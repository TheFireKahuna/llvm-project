//===-- X86WinCOFFStreamer.cpp - X86 Target WinCOFF Streamer ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/X86TargetInfo.h"
#include "X86BaseInfo.h"
#include "X86FixupKinds.h"
#include "X86MCAsmInfo.h"
#include "X86MCTargetDesc.h"
#include "X86TargetStreamer.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCWin64EH.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

namespace {
class X86WinCOFFStreamer : public MCWinCOFFStreamer {
  Win64EH::UnwindEmitter EHStreamer;

  // Where the last instruction ended.
  const MCFragment *LastFrag = nullptr;
  size_t LastEnd;
  // Whether the last instruction was a fused test, whose je or jne is due.
  bool AfterTest = false;
  // The register that the last instruction loaded a pointer into for the call
  // through it, which is due.
  MCRegister CallLoad;
  // The prefix-only instructions that end where the last instruction ended.
  unsigned PrefixRun = 0;
  // The fragment of the last load of a pointer that the linker binds, the
  // fixup of its field and the bytes of the prefix-only instructions after
  // it, until the instruction after it is emitted, and whether the load is a
  // fused read.
  MCFragment *ReadFrag = nullptr;
  size_t ReadFixup, ReadPrefixes;
  bool ReadIsFused;
  // The register that the last instruction wrote, where it was the load of a
  // pointer that is no fused read.
  MCRegister LastLoad;
  // The fragments that end with the load of a pointer for the call after it.
  SmallPtrSet<const MCFragment *, 0> CallLoads;
  // The label that the last instruction jumps to, a jcc that no label
  // follows. Where the last instruction is a jmp through a pointer that no
  // unwind frame covers, directly after such a jcc, the jcc's label, and the
  // fragment and fixup of the jmp's field.
  const MCSymbol *JccTarget = nullptr, *OverTarget = nullptr;
  MCFragment *OverFrag;
  size_t OverFixup;
  // The fragments that end with a jmp through a pointer that a jcc jumps over.
  SmallPtrSet<const MCFragment *, 0> OverJumps;

public:
  X86WinCOFFStreamer(MCContext &C, std::unique_ptr<MCAsmBackend> AB,
                     std::unique_ptr<MCCodeEmitter> CE,
                     std::unique_ptr<MCObjectWriter> OW)
      : MCWinCOFFStreamer(C, std::move(AB), std::move(CE), std::move(OW)) {
    if (C.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment() &&
        C.getTargetTriple().isX86_64()) {
      MCII.reset(getTheX86_64Target().createMCInstrInfo());
      // imul, then add.
      TlsScaleInsts = 2;
      FusedForms[size_t(COFF::FusedUse::Add)] =
          "lea of a constant directly after the load of a pointer";
      FusedForms[size_t(COFF::FusedUse::Access)] =
          "load or store through a register alone";
      FusedForms[size_t(COFF::FusedUse::Test)] =
          "test of a 64-bit register against itself";
    }
  }

  void reset() override {
    LastFrag = ReadFrag = nullptr;
    LastLoad = CallLoad = MCRegister();
    JccTarget = OverTarget = nullptr;
    CallLoads.clear();
    OverJumps.clear();
    PrefixRun = 0;
    AfterTest = false;
    MCWinCOFFStreamer::reset();
  }
  void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override;
  void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc()) override;
  void emitWinEHHandlerData(SMLoc Loc) override;
  void emitWindowsUnwindTables(WinEH::FrameInfo *Frame) override;
  void emitWindowsUnwindTables() override;
  void emitCVFPOData(const MCSymbol *ProcSym, SMLoc Loc) override;
  void finishImpl() override;
};

// A jcc that jumps over the jmp through a pointer directly after it, to the
// label directly after that, is a conditional tail call the other way round:
// the field of the jmp says so, and the linker writes the two as one jcc to
// a function in the image. A label between the two is another way to the
// jmp, which the linker could not keep.
void X86WinCOFFStreamer::emitLabel(MCSymbol *Symbol, SMLoc Loc) {
  if (Symbol == OverTarget && getCurrentFragment() == LastFrag &&
      getCurFragSize() == LastEnd) {
    MCFixup &Field = OverFrag->getFixups()[OverFixup];
    Field = MCFixup::create(
        Field.getOffset(),
        MCBinaryExpr::createAdd(
            Field.getValue(),
            MCConstantExpr::create(COFF::JumpOverJcc, getContext()),
            getContext(), Field.getLoc()),
        Field.getKind(), Field.isPCRel());
    OverJumps.insert(OverFrag);
    OverTarget = nullptr;
  }
  JccTarget = nullptr;
  if (CallLoad)
    getContext().reportError(Loc, "label between the load of a pointer for a "
                                  "call and the call");
  MCWinCOFFStreamer::emitLabel(Symbol, Loc);
}

void X86WinCOFFStreamer::emitInstruction(const MCInst &Inst,
                                         const MCSubtargetInfo &STI) {
  unsigned Opcode = Inst.getOpcode();
  bool Follows =
      getCurrentFragment() == LastFrag && getCurFragSize() == LastEnd;
  const MCSymbol *Over = Follows ? JccTarget : nullptr;
  JccTarget = OverTarget = nullptr;
  // On Windows Itanium and NT-POSIX the relocation type of a jmp, direct,
  // through a pointer or fused, states what the linker may do to it, which
  // the unwind data covering it decides. A jmp outside a frame may be deleted,
  // as may one that ends an epilogue where the unwinder is right at every
  // address with the jmp gone. Version 2's is, every distance it reads being
  // a relocation. Version 1's reads the code, and is where the epilogue is
  // one instruction that leaves the frame whole before it: the stack
  // adjustment of a frame without a push, or `leave`.
  auto MayDelete = [&] {
    const WinEH::FrameInfo *Frame = getCurrentWinFrameInfo();
    if (!Frame)
      return true;
    if (Frame->EpilogMap.empty() || Frame->Version > 2)
      return false;
    const WinEH::FrameInfo::Epilog &Epilog = Frame->EpilogMap.back().second;
    const MCFragment *F = getCurrentFragment();
    if (!isInEpilogCFI() && (Epilog.End->getFragment() != F ||
                             Epilog.End->getOffset() != getCurFragSize()))
      return false;
    if (Frame->Version == 2)
      return true;
    if (Epilog.Start->getFragment() == F &&
        Epilog.Start->getOffset() + 1 == getCurFragSize() &&
        F->getContents()[Epilog.Start->getOffset()] == '\xc9')
      return true;
    for (const WinEH::FrameInfo *I = Frame; I; I = I->ChainedParent)
      if (any_of(I->Instructions, [](const WinEH::Instruction &Code) {
            return Code.Operation == Win64EH::UOP_PushNonVol;
          }))
        return false;
    return true;
  };
  const MCSymbol *Load = nullptr;
  bool Read = false, Stated = false;
  MCRegister ForCall;
  // A fused site is one unit to padding, which goes before its marker.
  unsigned Part = X86_MC::FSP_None;
  MCSymbol *Marker = nullptr;
  if (MCII) {
    // The je or jne that alone reads the result of a fused test follows it
    // directly, so that the linker finds it, and the two are one unit to
    // padding.
    if (AfterTest) {
      if (Opcode == X86::JCC_1 && Follows &&
          (Inst.getOperand(1).getImm() == X86::COND_E ||
           Inst.getOperand(1).getImm() == X86::COND_NE))
        Part = X86_MC::FSP_Last;
      else
        getContext().reportError(Inst.getLoc(),
                                 "fused test is not followed by a je or jne");
      AfterTest = false;
    }
    // The call through the register that a load of a pointer wrote for it
    // follows the load directly: the linker writes a direct call over the
    // two, which are one unit to padding.
    if (CallLoad) {
      if (Opcode == X86::CALL64r && Follows && !Inst.getFlags() &&
          Inst.getOperand(0).getReg() == CallLoad)
        Part = X86_MC::FSP_Last;
      else
        getContext().reportError(Inst.getLoc(),
                                 "load of a pointer for a call is not "
                                 "followed by the call through its register");
      CallLoad = MCRegister();
    }
    unsigned RegOp;
    COFF::FusedUse Use = X86::getFusedUse(Inst, MCII->get(Opcode), RegOp);
    // A fused add is the lea of a constant to the register that the load of
    // a pointer directly before it wrote, which the linker makes the lea of
    // what the pointer names plus the constant. The index of its marker
    // states the length of the lea and what it adds.
    if (Use == COFF::FusedUse::Add) {
      int64_t Addend = Inst.getOperand(1 + X86::AddrDisp).getImm();
      if (!Follows || LastLoad != Inst.getOperand(0).getReg()) {
        Use = COFF::FusedUse::None;
      } else if (FusedSite.Offset && FusedSite.Use == Use) {
        SmallString<16> Code;
        SmallVector<MCFixup, 1> Fixups;
        getAssembler().getEmitter().encodeInstruction(Inst, Code, Fixups, STI);
        MCObjectStreamer::emitRelocDirective(
            *FusedSite.Offset, "IMAGE_REL_LLVM_FUSED_ADD",
            MCConstantExpr::create(
                Code.size() | (uint32_t(Addend) << 1 ^ uint32_t(Addend >> 63))
                                  << 4,
                getContext()),
            FusedSite.Loc);
      }
    }
    // A marked site is one instruction, and with the jcc after it two where
    // it is a test.
    if (FusedSite.Offset && Use == FusedSite.Use) {
      AfterTest = Use == COFF::FusedUse::Test;
      Part = X86_MC::FSP_First | (AfterTest ? 0 : X86_MC::FSP_Last);
    }
    // The compare of a pointer with zero is a fused test where its relocation
    // states so.
    bool IsTest = false;
    if ((Opcode == X86::CMP64mi8 || Opcode == X86::CMP64mi32) &&
        Inst.getOperand(X86::AddrDisp).isExpr() &&
        Inst.getOperand(X86::AddrBaseReg).getReg() == X86::RIP &&
        !Inst.getOperand(X86::AddrSegmentReg).getReg() &&
        Inst.getOperand(X86::AddrNumOperands).isImm() &&
        !Inst.getOperand(X86::AddrNumOperands).getImm())
      if (auto *Pointer = dyn_cast<MCSymbolRefExpr>(
              Inst.getOperand(X86::AddrDisp).getExpr()))
        IsTest = Pointer->getSpecifier() == X86::S_COFF_FUSED;
    if (IsTest) {
      AfterTest = true;
      Part = X86_MC::FSP_First;
    }
    // The linker may resize a 64-bit load through RIP of a pointer that it
    // binds, which only the pointer's name tells an assembler; a load of any
    // other symbol keeps its size. A mov of a pointer through RIP alone loads
    // it, and is a fused read where its relocation states so.
    if (Opcode == X86::MOV64rm && Inst.getOperand(1 + X86::AddrDisp).isExpr())
      if (auto *Pointer = dyn_cast<MCSymbolRefExpr>(
              Inst.getOperand(1 + X86::AddrDisp).getExpr())) {
        bool Rip = Inst.getOperand(1 + X86::AddrBaseReg).getReg() == X86::RIP;
        // A load for the call after it is a site with that call.
        if (Pointer->getSpecifier() == X86::S_COFF_LOAD_CALL) {
          ForCall = Inst.getOperand(0).getReg();
          Part = X86_MC::FSP_First;
        }
        Stated =
            ForCall ||
            (Rip && COFF::isBoundPointerName(Pointer->getSymbol().getName()));
        Read = Pointer->getSpecifier() == X86::S_COFF_FUSED;
        if (Read || (Pointer->getSpecifier() == X86::S_None && Rip &&
                     !Inst.getOperand(1 + X86::AddrSegmentReg).getReg()))
          Load = &Pointer->getSymbol();
      }
    if (Read || IsTest)
      MovingSections.insert(getCurrentSectionOnly());
    Marker = checkFused(Inst, Use, RegOp, Load, Read || IsTest);
  }
  // The field of a fused read holds twice the length of the instruction after
  // it, with its prefixes, plus one where that transfers control or may fuse
  // with a branch that does: zero unless the instruction is seen to follow
  // the read and to have that length, and where it has a segment override
  // other than CS, which the linker's CS overrides would contradict. The
  // field of a load of a pointer that the linker binds says that the flags
  // are dead at the load where the instruction seen to follow it writes them
  // all and reads none.
  scope_exit Emitted([&, Frag = getCurrentFragment(),
                      Begin = getCurFragSize()] {
    MCFragment *After = getCurrentFragment();
    size_t End = getCurFragSize();
    if (ReadFrag) {
      const MCInstrDesc &Desc = MCII->get(Opcode);
      bool IsPrefix = X86II::isPrefix(Desc.TSFlags);
      if (!Follows || After != Frag ||
          (IsPrefix &&
           is_contained({X86::SS_PREFIX, X86::DS_PREFIX, X86::ES_PREFIX,
                         X86::FS_PREFIX, X86::GS_PREFIX},
                        Opcode))) {
        ReadFrag = nullptr;
      } else if (IsPrefix) {
        ReadPrefixes += End - Begin;
      } else {
        MCFixup &Field = ReadFrag->getFixups()[ReadFixup];
        X86::FirstMacroFusionInstKind Fuses =
            X86::classifyFirstOpcodeInMacroFusion(Opcode);
        bool Transfers = Desc.isBranch() || Desc.isCall() || Desc.isReturn() ||
                         Fuses != X86::FirstMacroFusionInstKind::Invalid;
        uint64_t States = 0;
        if (ReadIsFused && X86_MC::determinePaddingPrefix(Inst, *MCII, STI) ==
                               X86::CS_Encoding)
          States = (ReadPrefixes + End - Begin) << 1 | Transfers;
        // A test, a compare, an and, an add and a sub write every flag.
        if (is_contained({X86::FirstMacroFusionInstKind::Test,
                          X86::FirstMacroFusionInstKind::Cmp,
                          X86::FirstMacroFusionInstKind::And,
                          X86::FirstMacroFusionInstKind::AddSub},
                         Fuses))
          States |= COFF::LoadFlagsDead;
        if (States)
          Field =
              MCFixup::create(Field.getOffset(),
                              MCBinaryExpr::createAdd(
                                  Field.getValue(),
                                  MCConstantExpr::create(States, getContext()),
                                  getContext(), Field.getLoc()),
                              Field.getKind(), Field.isPCRel());
        ReadFrag = nullptr;
      }
    }
    if ((Read || Stated) && !After->getFixups().empty() &&
        After->getFixups().back().getOffset() + 4 == End) {
      ReadFrag = After;
      ReadFixup = After->getFixups().size() - 1;
      ReadPrefixes = 0;
      ReadIsFused = Read;
    }
    // The linker may resize a 64-bit load of a pointer, a fused read among
    // them, and a site of several instructions ends with its last. The code
    // emitter states the other instructions it may resize; a load is stated
    // here, once the field of a fused read has been found.
    if (MCII) {
      if (ForCall)
        CallLoads.insert(After);
      finishInstruction(Stated || Read || (Part & X86_MC::FSP_Last), Frag);
      After = getCurrentFragment();
      End = getCurFragSize();
    }
    LastLoad = Load && !Read ? Inst.getOperand(0).getReg() : MCRegister();
    if (MCII && Opcode == X86::JCC_1 && Inst.getOperand(0).isExpr()) {
      if (auto *To = dyn_cast<MCSymbolRefExpr>(Inst.getOperand(0).getExpr()))
        JccTarget = &To->getSymbol();
    } else if (Over && !getCurrentWinFrameInfo() &&
               !Frag->getFixups().empty() &&
               Frag->getFixups().back().getKind() ==
                   X86::reloc_riprel_4byte_tail) {
      OverTarget = Over;
      OverFrag = Frag;
      OverFixup = Frag->getFixups().size() - 1;
    }
    PrefixRun = MCII && X86II::isPrefix(MCII->get(Opcode).TSFlags)
                    ? (Follows ? PrefixRun : 0) + 1
                    : 0;
    LastFrag = After;
    LastEnd = End;
    CallLoad = ForCall;
  });

  // A jmp through memory states the prefixes before it, which the linker
  // cannot see from its site: a prefix-only instruction's among them, and
  // data, whose bytes may be prefixes.
  unsigned Flags = Inst.getFlags();
  if (Opcode == X86::JMP64m || Opcode == X86::JMP64m_REX ||
      Opcode == X86::JMP64m_NT) {
    if (PrefixRun == 1 && Follows)
      Flags |= X86::IP_AFTER_PREFIX;
    else if (PrefixRun || (!Follows && getCurFragSize()))
      Flags |= X86::IP_AFTER_PREFIXES;
  }

  if ((Opcode == X86::JMP_1 || Opcode == X86::JMP64m) &&
      STI.hasFeature(X86::Is64Bit) &&
      STI.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment() &&
      MayDelete())
    Flags |= X86::IP_TAIL_JUMP;
  if (Flags != Inst.getFlags()) {
    MCInst Jump(Inst);
    Jump.setFlags(Flags);
    return X86_MC::emitInstruction(*this, Jump, STI, Part, Marker);
  }
  X86_MC::emitInstruction(*this, Inst, STI, Part, Marker);
}

void X86WinCOFFStreamer::emitWinEHHandlerData(SMLoc Loc) {
  MCStreamer::emitWinEHHandlerData(Loc);

  // We have to emit the unwind info now, because this directive
  // actually switches to the .xdata section.
  if (WinEH::FrameInfo *CurFrame = getCurrentWinFrameInfo()) {
    // Handlers are always associated with the parent frame.
    CurFrame = CurFrame->ChainedParent ? CurFrame->ChainedParent : CurFrame;
    EHStreamer.EmitUnwindInfo(*this, CurFrame, /* HandlerData = */ true);
  }
}

void X86WinCOFFStreamer::emitWindowsUnwindTables(WinEH::FrameInfo *Frame) {
  EHStreamer.EmitUnwindInfo(*this, Frame, /* HandlerData = */ false);
}

void X86WinCOFFStreamer::emitWindowsUnwindTables() {
  if (!getNumWinFrameInfos())
    return;
  EHStreamer.Emit(*this);
}

void X86WinCOFFStreamer::emitCVFPOData(const MCSymbol *ProcSym, SMLoc Loc) {
  X86TargetStreamer *XTS =
      static_cast<X86TargetStreamer *>(getTargetStreamer());
  XTS->emitFPOData(ProcSym, Loc);
}

void X86WinCOFFStreamer::finishImpl() {
  if (AfterTest)
    getContext().reportError(SMLoc(),
                             "fused test is not followed by a je or jne");
  if (CallLoad)
    getContext().reportError(SMLoc(), "load of a pointer for a call is not "
                                      "followed by the call through its "
                                      "register");
  // The linker moves the code of a section only after a site that says so.
  // Any other it only compacts: it writes a direct call in five bytes over a
  // load and the call through its register, deletes a jmp to another section
  // that ends the section, and writes every other site in its assembled
  // length. So nothing in such a section is the linker's to resize but the
  // fragments of those loads and of that jmp, and of the jumps whose width
  // the layout leaves to the linker. The assembler resolves every distance,
  // chooses every other width, and states as a span each value that spans
  // one of them.
  for (MCSection &Sec : getAssembler()) {
    if (!MCII || !Sec.isLinkerRelaxable() || MovingSections.contains(&Sec))
      continue;
    MCFragment *Last = nullptr;
    unsigned First = -1u;
    // The bytes of a fragment that stay in every link, of which no layout
    // has fewer: a direct call takes five from a load and its call, a direct
    // jmp of two bytes four from one through a pointer, and padding may go.
    // A jmp that a jcc jumps over keeps its bytes: bound, the two are one jcc
    // of 32 bits and a no-operation.
    auto IsMemJump = [&](const MCFragment &F) {
      return !F.getFixups().empty() && !OverJumps.contains(&F) &&
             F.getFixups().back().getOffset() + 4 == F.getSize() &&
             (F.getFixups().back().getKind() == X86::reloc_riprel_4byte_jump ||
              F.getFixups().back().getKind() ==
                  X86::reloc_riprel_4byte_jump_p1 ||
              F.getFixups().back().getKind() == X86::reloc_riprel_4byte_tail);
    };
    auto Stays = [&](const MCFragment &F) -> uint64_t {
      if (F.getKind() == MCFragment::FT_Align ||
          F.getKind() == MCFragment::FT_BoundaryAlign)
        return F.getFixedSize();
      return F.getSize() - (CallLoads.contains(&F) ? 5 : IsMemJump(F) ? 4 : 0);
    };
    uint64_t After = 0;
    for (MCFragment &F : Sec) {
      F.setLinkerRelaxable(CallLoads.contains(&F));
      if (F.isLinkerRelaxable())
        First = std::min(First, F.getLayoutOrder());
      if (F.getKind() != MCFragment::FT_Data || F.getSize())
        Last = &F;
      After += Stays(F);
    }
    // A jump to another section, and one through a pointer that the linker
    // may bind, reach with 8 bits where the linker lays the target's section
    // out near enough, which only it knows: its width is the linker's from
    // where at most their reach stays between the jump and its section's
    // end.
    for (MCFragment &F : Sec) {
      After -= Stays(F);
      if (After > 127)
        continue;
      bool Far = IsMemJump(F);
      if (F.getKind() == MCFragment::FT_Relaxable &&
          (F.getOpcode() == X86::JMP_1 || F.getOpcode() == X86::JCC_1)) {
        const MCOperand &To = F.getInst().getOperand(0);
        auto *Ref =
            To.isExpr() ? dyn_cast<MCSymbolRefExpr>(To.getExpr()) : nullptr;
        Far =
            Ref && (!Ref->getSymbol().isInSection() ||
                    (&Ref->getSymbol().getSection() != &Sec &&
                     Ref->getSymbol().getSection().getName() == Sec.getName()));
      }
      if (Far) {
        F.setLinkerRelaxable();
        First = std::min(First, F.getLayoutOrder());
      }
    }
    bool Tail = Last && any_of(Last->getFixups(), [](const MCFixup &Fixup) {
                  return Fixup.getKind() == X86::reloc_tail_4byte_pcrel ||
                         Fixup.getKind() == X86::reloc_riprel_4byte_tail;
                });
    // A jmp whose width the assembler has yet to choose.
    if (Last && Last->getKind() == MCFragment::FT_Relaxable)
      if (MCInst Jump = Last->getInst(); Jump.getFlags() & X86::IP_TAIL_JUMP) {
        const MCOperand &To = Jump.getOperand(0);
        auto *Ref =
            To.isExpr() ? dyn_cast<MCSymbolRefExpr>(To.getExpr()) : nullptr;
        Tail = !Ref || !Ref->getSymbol().isInSection() ||
               &Ref->getSymbol().getSection() != &Sec;
      }
    if (Tail)
      Last->setLinkerRelaxable();
    Sec.setFirstLinkerRelaxable(Tail ? std::min(First, Last->getLayoutOrder())
                                     : First);
    // A jump to another section may yet be the linker's to narrow.
    getWriter().Spans[{&Sec, &Sec}];
  }
  emitFrames();
  emitWindowsUnwindTables();

  MCWinCOFFStreamer::finishImpl();
}
} // namespace

MCStreamer *
llvm::createX86WinCOFFStreamer(MCContext &C, std::unique_ptr<MCAsmBackend> &&AB,
                               std::unique_ptr<MCObjectWriter> &&OW,
                               std::unique_ptr<MCCodeEmitter> &&CE) {
  return new X86WinCOFFStreamer(C, std::move(AB), std::move(CE), std::move(OW));
}
