//===-- X86WinCOFFStreamer.cpp - X86 Target WinCOFF Streamer ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/X86TargetInfo.h"
#include "X86BaseInfo.h"
#include "X86MCAsmInfo.h"
#include "X86MCTargetDesc.h"
#include "X86TargetStreamer.h"
#include "llvm/ADT/ScopeExit.h"
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
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

namespace {
class X86WinCOFFStreamer : public MCWinCOFFStreamer {
  Win64EH::UnwindEmitter EHStreamer;

  // Where the last instruction ended.
  const MCFragment *LastFrag = nullptr;
  size_t LastEnd;
  // The segment overrides of the fused call whose marker is pending, and the
  // int3 that the last fused jump still lacks.
  unsigned Pads = 0, Int3s = 0;
  // The prefix-only instructions that end where the last instruction ended.
  unsigned PrefixRun = 0;
  // The last fused read's fragment, the fixup of its field and its end, until
  // the instruction after it is emitted.
  MCFragment *ReadFrag = nullptr;
  size_t ReadFixup, ReadEnd;

public:
  X86WinCOFFStreamer(MCContext &C, std::unique_ptr<MCAsmBackend> AB,
                     std::unique_ptr<MCCodeEmitter> CE,
                     std::unique_ptr<MCObjectWriter> OW)
      : MCWinCOFFStreamer(C, std::move(AB), std::move(CE), std::move(OW)) {
    if (C.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment() &&
        C.getTargetTriple().isX86_64()) {
      MCII.reset(getTheX86_64Target().createMCInstrInfo());
      FusedForms[0] = "six-byte call through a register";
      FusedForms[1] = "seven-byte jmp through a register";
    }
  }

  void reset() override {
    LastFrag = ReadFrag = nullptr;
    Pads = Int3s = PrefixRun = 0;
    MCWinCOFFStreamer::reset();
  }
  void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override;
  void emitWinEHHandlerData(SMLoc Loc) override;
  void emitWindowsUnwindTables(WinEH::FrameInfo *Frame) override;
  void emitWindowsUnwindTables() override;
  void emitCVFPOData(const MCSymbol *ProcSym, SMLoc Loc) override;
  void finishImpl() override;
};

void X86WinCOFFStreamer::emitInstruction(const MCInst &Inst,
                                         const MCSubtargetInfo &STI) {
  unsigned Opcode = Inst.getOpcode();
  bool Follows =
      getCurrentFragment() == LastFrag && getCurFragSize() == LastEnd;
  const MCSymbol *Read = nullptr;
  // A fused site is one unit to padding, which goes before its marker.
  unsigned Part = X86_MC::FSP_None;
  MCSymbol *Marker = nullptr;
  if (MCII) {
    // A fused call is a call through a register behind the CS overrides that
    // make it six bytes. A fused jump is a jmp with REX.W through a register
    // before the int3 that make it seven. The linker writes a direct call or
    // jump over either.
    if (Int3s && (Opcode != X86::INT3 || !Follows)) {
      reportMisplacedFusedSite();
      Int3s = 0;
    } else if (Int3s) {
      Part = X86_MC::FSP_Inside | (--Int3s ? 0 : X86_MC::FSP_Last);
    }
    bool IsSite = false, IsPad = false;
    if (FusedSite.Offset) {
      unsigned Reg = Inst.getNumOperands() && Inst.getOperand(0).isReg()
                         ? getContext().getRegisterInfo()->getEncodingValue(
                               Inst.getOperand(0).getReg())
                         : 0;
      if (FusedSite.IsJump) {
        IsSite =
            Opcode == X86::JMP64r_REX ||
            (Opcode == X86::JMP64r && (Inst.getFlags() & X86::IP_HAS_REX_W));
        Int3s = IsSite ? 4 - (Reg >= 16) : 0;
      } else {
        IsPad = Opcode == X86::CS_PREFIX && (!Pads || Follows);
        IsSite = Opcode == X86::CALL64r && Follows &&
                 Pads + 2 + (Reg >= 8) + (Reg >= 16) == 6;
      }
    }
    if (IsPad || IsSite)
      Part = X86_MC::FSP_Inside |
             ((IsPad ? !Pads : FusedSite.IsJump) ? X86_MC::FSP_First : 0) |
             (IsSite && !FusedSite.IsJump ? X86_MC::FSP_Last : 0);
    Pads = IsPad ? Pads + 1 : 0;
    // A fused read is a mov of a pointer, whose form its relocation states.
    if (Opcode == X86::MOV64rm && Inst.getOperand(1 + X86::AddrDisp).isExpr())
      if (auto *Pointer = dyn_cast<MCSymbolRefExpr>(
              Inst.getOperand(1 + X86::AddrDisp).getExpr());
          Pointer && Pointer->getSpecifier() == X86::S_COFF_FUSED)
        Read = &Pointer->getSymbol();
    Marker = checkFused(Inst, Read, IsSite, IsPad);
  }
  // The field of a fused read holds twice the length of the instruction after
  // it, with its prefixes, plus one where that transfers control or may fuse
  // with a branch that does: zero unless the instruction is seen to follow
  // the read and to have that length, and where it has a segment override
  // other than CS, which the linker's CS overrides would contradict.
  scope_exit Emitted([&] {
    MCFragment *After = getCurrentFragment();
    size_t End = getCurFragSize();
    if (ReadFrag) {
      const MCInstrDesc &Desc = MCII->get(Opcode);
      bool IsPrefix = X86II::isPrefix(Desc.TSFlags);
      if (!Follows || After != ReadFrag ||
          (IsPrefix
               ? is_contained({X86::SS_PREFIX, X86::DS_PREFIX, X86::ES_PREFIX,
                               X86::FS_PREFIX, X86::GS_PREFIX},
                              Opcode)
               : X86_MC::determinePaddingPrefix(Inst, *MCII, STI) !=
                     X86::CS_Encoding)) {
        ReadFrag = nullptr;
      } else if (!IsPrefix) {
        MCFixup &Field = ReadFrag->getFixups()[ReadFixup];
        bool Transfers = Desc.isBranch() || Desc.isCall() || Desc.isReturn() ||
                         X86::classifyFirstOpcodeInMacroFusion(Opcode) !=
                             X86::FirstMacroFusionInstKind::Invalid;
        Field = MCFixup::create(
            Field.getOffset(),
            MCBinaryExpr::createAdd(
                Field.getValue(),
                MCConstantExpr::create((End - ReadEnd) << 1 | Transfers,
                                       getContext()),
                getContext(), Field.getLoc()),
            Field.getKind(), Field.isPCRel());
        ReadFrag = nullptr;
      }
    }
    if (Read && !After->getFixups().empty() &&
        After->getFixups().back().getOffset() + 4 == End) {
      ReadFrag = After;
      ReadFixup = After->getFixups().size() - 1;
      ReadEnd = End;
    }
    PrefixRun = MCII && X86II::isPrefix(MCII->get(Opcode).TSFlags)
                    ? (Follows ? PrefixRun : 0) + 1
                    : 0;
    LastFrag = After;
    LastEnd = End;
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

  // On Windows Itanium and NT-POSIX the relocation type of a jmp states what
  // the linker may do to it, which the unwind data covering it decides. A jmp
  // outside a frame may be deleted, as may one that ends an epilogue where
  // the unwinder is right at every address with the jmp gone. Version 2's is,
  // every distance it reads being a relocation. Version 1's reads the code,
  // and is where the epilogue is one instruction that leaves the frame whole
  // before it: the stack adjustment of a frame without a push, or `leave`.
  if (Inst.getOpcode() == X86::JMP_1 && STI.hasFeature(X86::Is64Bit) &&
      STI.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
    const WinEH::FrameInfo *Frame = getCurrentWinFrameInfo();
    auto EndsDeletableEpilog = [&] {
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
    if (!Frame || EndsDeletableEpilog())
      Flags |= X86::IP_TAIL_JUMP;
  }
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
  if (Int3s)
    reportMisplacedFusedSite();
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
