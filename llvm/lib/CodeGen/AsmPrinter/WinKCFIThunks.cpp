//===- WinKCFIThunks.cpp - KCFI per-type thunk emission -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file holds the target-independent driver that emits the per-type KCFI
// thunks the CFGuard pass routes a COFF module's indirect calls with a KCFI
// type through, when the prefixes carry a marker. It owns the scan of the
// thunk declarations, the tables of thunk and routine kinds, the weak aliases
// of the mismatch routines, the code-range default, the open types and their
// lists, and the trap. The per-target hooks on AsmPrinter emit the bodies.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/KCFIHash.h"

using namespace llvm;


void AsmPrinter::emitKCFIFunctionStart(MCSymbol *Sym, int Selection,
                                       Align Alignment) {
  const MCSubtargetInfo &STI = TM.getMCSubtargetInfo();
  OutStreamer->switchSection(OutContext.getCOFFSection(
      ".text",
      COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE |
          COFF::IMAGE_SCN_MEM_READ | COFF::IMAGE_SCN_LNK_COMDAT,
      Sym->getName(), Selection));
  OutStreamer->beginCOFFSymbolDef(Sym);
  OutStreamer->emitCOFFSymbolStorageClass(COFF::IMAGE_SYM_CLASS_EXTERNAL);
  OutStreamer->emitCOFFSymbolType(COFF::IMAGE_SYM_DTYPE_FUNCTION
                                  << COFF::SCT_COMPLEX_TYPE_SHIFT);
  OutStreamer->endCOFFSymbolDef();
  OutStreamer->emitSymbolAttribute(Sym, MCSA_Global);
  OutStreamer->emitCodeAlignment(Alignment, STI);
  OutStreamer->emitLabel(Sym);
}

namespace {
// The !kcfi_thunk metadata a per-type thunk declaration carries.
struct ThunkMD {
  KCFIThunkKind Kind;
  uint32_t Type;
  unsigned Flags;
};
} // namespace

static std::optional<ThunkMD> getThunkMD(const Function &F) {
  const MDNode *MD = F.getMetadata("kcfi_thunk");
  if (!MD)
    return std::nullopt;
  auto Op = [&](unsigned I) {
    return mdconst::extract<ConstantInt>(MD->getOperand(I))->getZExtValue();
  };
  return ThunkMD{KCFIThunkKind(Op(0)), uint32_t(Op(1)), unsigned(Op(2))};
}

// Makes both range bounds weak aliases of __llvm_code_empty, a byte of
// read-only data in a COMDAT, so that an image whose linker does not define
// them has an empty range. The default is defined in a section, so that
// references stay against the bounds, which a linker can then define; those to
// an alias of an undefined symbol are relocated against that symbol.
static void emitKCFICodeRangeDefault(MCStreamer &OS, MCContext &Ctx,
                                     MCSymbol *Start, MCSymbol *End) {
  MCSymbol *Empty = Ctx.getOrCreateSymbol(COFF::KCFICodeEmpty);
  for (MCSymbol *Sym : {Start, End}) {
    OS.emitSymbolAttribute(Sym, MCSA_Weak);
    OS.emitAssignment(Sym, MCSymbolRefExpr::create(Empty, Ctx));
  }
  OS.switchSection(Ctx.getCOFFSection(
      ".rdata",
      COFF::IMAGE_SCN_CNT_INITIALIZED_DATA | COFF::IMAGE_SCN_MEM_READ |
          COFF::IMAGE_SCN_LNK_COMDAT,
      Empty->getName(), COFF::IMAGE_COMDAT_SELECT_ANY));
  OS.emitSymbolAttribute(Empty, MCSA_Global);
  OS.emitLabel(Empty);
  OS.emitIntValue(0, 1);
}

void AsmPrinter::emitKCFIThunks(Module &M) {
  ArrayRef<KCFIRoutineKind> Routines = getKCFIRoutineKinds();
  if (Routines.empty())
    return;
  const ConstantInt *Marker = mdconst::extract_or_null<ConstantInt>(
      M.getModuleFlag("function-type-prefix"));
  if (!Marker)
    return;
  int64_t PrefixNops = 0;
  if (auto *MD =
          mdconst::extract_or_null<ConstantInt>(M.getModuleFlag("kcfi-offset")))
    PrefixNops = MD->getZExtValue();
  uint64_t Pattern = getTypePrefixPattern(Marker->getZExtValue());

  // The check routine, which a vfn thunk and a type-0 member thunk share.
  const KCFIRoutineKind *CheckRoutine =
      llvm::find_if(Routines, [](const KCFIRoutineKind &R) {
        return R.Kind == KCFIThunkCheck;
      });
  assert(CheckRoutine != Routines.end());

  bool AnyThunk = false;
  MCSymbol *TrapFn = OutContext.getOrCreateSymbol(COFF::KCFITrap);
  MCSymbol *CodeStart = nullptr;
  MCSymbol *CodeEnd = nullptr;
  auto ensureRange = [&] {
    if (CodeStart)
      return;
    CodeStart = OutContext.getOrCreateSymbol(COFF::KCFICodeStart);
    CodeEnd = OutContext.getOrCreateSymbol(COFF::KCFICodeEnd);
    emitKCFICodeRangeDefault(*OutStreamer, OutContext, CodeStart, CodeEnd);
  };

  // The ordinary, local and vfn thunks, in the same order as a type's: all the
  // non-local routines, then the local ones, then the vfn check.
  struct ThunkKind {
    const KCFIRoutineKind *Routine;
    bool Local;
    bool Vfn;
  };
  SmallVector<ThunkKind, 5> Kinds;
  for (const KCFIRoutineKind &R : Routines)
    Kinds.push_back({&R, /*Local=*/false, /*Vfn=*/false});
  for (const KCFIRoutineKind &R : Routines)
    Kinds.push_back({&R, /*Local=*/true, /*Vfn=*/false});
  Kinds.push_back({CheckRoutine, /*Local=*/false, /*Vfn=*/true});

  for (const ThunkKind &Kind : Kinds) {
    for (const Function &F : M) {
      std::optional<ThunkMD> MD = getThunkMD(F);
      if (!F.isDeclaration() || MD == std::nullopt ||
          (MD->Flags & KCFIThunkMember) || MD->Kind != Kind.Routine->Kind ||
          bool(MD->Flags & KCFIThunkLocal) != Kind.Local ||
          bool(MD->Flags & KCFIThunkVfn) != Kind.Vfn)
        continue;
      MCSymbol *Thunk = getSymbol(&F);
      if (F.use_empty())
        continue;
      AnyThunk = true;
      if (Kind.Local)
        ensureRange();

      std::string Hex = utohexstr(MD->Type, /*LowerCase=*/true, /*Width=*/8);
      // A type's local and ordinary thunks share its mismatch routine, which
      // this module defines if it opens the type.
      MCSymbol *Mismatch =
          OutContext.getOrCreateSymbol(Kind.Routine->MismatchPrefix + Hex);
      if (!Mismatch->isVariable()) {
        OutStreamer->emitSymbolAttribute(Mismatch, MCSA_Weak);
        OutStreamer->emitAssignment(
            Mismatch, MCSymbolRefExpr::create(TrapFn, OutContext));
      }

      emitKCFIFunctionStart(Thunk, COFF::IMAGE_COMDAT_SELECT_ANY, Align(16));

      emitKCFIThunk({Kind.Routine, MD->Type, Pattern, PrefixNops, Kind.Local,
                     Kind.Vfn, Mismatch, /*Miss=*/nullptr, /*Tags=*/{},
                     Kind.Local ? CodeStart : nullptr,
                     Kind.Local ? CodeEnd : nullptr});
    }
  }

  // The trap is the default of every type's mismatch routine, a static
  // scanner's fall-through, and a type-0 member thunk's miss.
  if (!AnyThunk)
    return;

  emitKCFIFunctionStart(TrapFn, COFF::IMAGE_COMDAT_SELECT_ANY, Align(16));
  emitKCFIFastFail();

}
