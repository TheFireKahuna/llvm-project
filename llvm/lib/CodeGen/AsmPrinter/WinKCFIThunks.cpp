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

MapVector<uint32_t, AsmPrinter::KCFIOpenType>
AsmPrinter::getKCFIOpenTypes(const Module &M) const {
  // A module opens a type statically by taking the address of a known import
  // of the type, which is then the only kind of foreign target the image can
  // hold for it, and dynamically where a pointer of the type may come from
  // foreign code.
  MapVector<uint32_t, KCFIOpenType> OpenTypes;
  for (const Function &F : M)
    if (const MDNode *MD = F.getMetadata(LLVMContext::MD_kcfi_type))
      if (F.isDeclaration() && F.hasDLLImportStorageClass() &&
          F.hasMetadata("kcfi_import") && F.hasAddressTaken())
        OpenTypes[mdconst::extract<ConstantInt>(MD->getOperand(0))
                      ->getZExtValue()]
            .Imports.push_back(&F);
  if (const NamedMDNode *Dynamic = M.getNamedMetadata("kcfi.dynamic"))
    for (const MDNode *MD : Dynamic->operands())
      OpenTypes[mdconst::extract<ConstantInt>(MD->getOperand(0))
                    ->getZExtValue()]
          .Dynamic = true;
  return OpenTypes;
}

void AsmPrinter::emitKCFIList(MCSymbol *List, uint32_t Type,
                              ArrayRef<const Function *> Imports) {
  // The pieces of a type's list are in sections that the linker merges in the
  // order of their names: the head, in a COMDAT, which the type's open routine
  // refers to past its first word; each object's entries; and the trailer,
  // kept with the head, whose odd word ends the list. Both hold a word unique
  // to the type, so that no two types' pieces are folded, and the entries are
  // in no COMDAT, so that none is.
  std::string Prefix = COFF::KCFIListSectionPrefix +
                       utohexstr(Type, /*LowerCase=*/true, /*Width=*/8) + "_";
  unsigned Characteristics =
      COFF::IMAGE_SCN_CNT_INITIALIZED_DATA | COFF::IMAGE_SCN_MEM_READ;
  OutStreamer->switchSection(OutContext.getCOFFSection(
      Prefix + "a", Characteristics | COFF::IMAGE_SCN_LNK_COMDAT,
      List->getName(), COFF::IMAGE_COMDAT_SELECT_ANY));
  OutStreamer->emitValueToAlignment(Align(8));
  OutStreamer->emitSymbolAttribute(List, MCSA_Global);
  OutStreamer->emitLabel(List);
  OutStreamer->emitInt64(Type);

  // An entry is the address of a cell holding a valid target. An import's is
  // its import address table entry, which holds the address the loader bound.
  // Where static data holds the import's thunk instead, which only the linker
  // knows, the linker lists a cell holding the thunk, as the records ask.
  if (!Imports.empty()) {
    OutStreamer->switchSection(
        OutContext.getCOFFSection(Prefix + "m", Characteristics));
    OutStreamer->emitValueToAlignment(Align(8));
    for (const Function *F : Imports)
      OutStreamer->emitValue(
          MCSymbolRefExpr::create(
              OutContext.getOrCreateSymbol("__imp_" + getSymbol(F)->getName()),
              OutContext),
          8);
    OutStreamer->emitCOFFLinkFact(COFF::LinkRecordKCFIImportLists);
  }

  OutStreamer->switchSection(OutContext.getCOFFSection(
      Prefix + "z", Characteristics | COFF::IMAGE_SCN_LNK_COMDAT,
      List->getName(), COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE));
  OutStreamer->emitValueToAlignment(Align(8));
  OutStreamer->emitInt64(uint64_t(Type) << 1 | 1);
}

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

  MapVector<uint32_t, KCFIOpenType> OpenTypes = getKCFIOpenTypes(M);
  // A module defines the scanners only if it opens a type, whose mismatch
  // routines pass their lists to them. A module that only calls thunks
  // references no scanner, so emitting them there would be dead COMDATs.
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
      if (!Mismatch->isVariable() && !OpenTypes.count(MD->Type)) {
        OutStreamer->emitSymbolAttribute(Mismatch, MCSA_Weak);
        OutStreamer->emitAssignment(
            Mismatch, MCSymbolRefExpr::create(TrapFn, OutContext));
      }

      emitKCFIFunctionStart(Thunk, COFF::IMAGE_COMDAT_SELECT_ANY, Align(16));
      // A linker may replace an ordinary thunk with a form of its own, which
      // it builds from these facts.
      if (!Kind.Local)
        OutStreamer->emitCOFFKCFIThunk(Thunk,
                                       Kind.Vfn ? COFF::LinkKCFIThunkVfnCheck
                                       : Kind.Routine->Kind == KCFIThunkDispatch
                                           ? COFF::LinkKCFIThunkDispatch
                                           : COFF::LinkKCFIThunkCheck,
                                       MD->Type, Marker->getZExtValue(),
                                       PrefixNops * getKCFIPrefixByteScale(),
                                       Mismatch);

      emitKCFIThunk({Kind.Routine, MD->Type, Pattern, PrefixNops, Kind.Local,
                     Kind.Vfn, Mismatch, /*Miss=*/nullptr, /*Tags=*/{},
                     Kind.Local ? CodeStart : nullptr,
                     Kind.Local ? CodeEnd : nullptr});
    }
  }

  for (const KCFIRoutineKind &Routine : Routines) {
    if (OpenTypes.empty())
      break;
    for (bool Dynamic : {false, true}) {
      emitKCFIFunctionStart(
          OutContext.getOrCreateSymbol(Dynamic ? Routine.DynamicScanner
                                               : Routine.Scanner),
          COFF::IMAGE_COMDAT_SELECT_ANY, Align(16));
      emitKCFIScanner(Routine, Dynamic, Pattern, PrefixNops);
    }
  }

  // The trap is the default of every type's mismatch routine, a static
  // scanner's fall-through, and a type-0 member thunk's miss.
  if (!AnyThunk && OpenTypes.empty())
    return;

  emitKCFIFunctionStart(TrapFn, COFF::IMAGE_COMDAT_SELECT_ANY, Align(16));
  emitKCFIFastFail();

  for (const auto &[Type, Open] : OpenTypes) {
    std::string Hex = utohexstr(Type, /*LowerCase=*/true, /*Width=*/8);
    MCSymbol *List = OutContext.getOrCreateSymbol(COFF::KCFIListPrefix + Hex);
    for (const KCFIRoutineKind &Routine : Routines) {
      // A dynamic opener's routine ends with a trap, so that the linker's
      // choice of the largest definition prefers it to a static opener's. A
      // routine is only reached by a jump on a mismatch, so it is not aligned
      // beyond an instruction.
      emitKCFIFunctionStart(
          OutContext.getOrCreateSymbol(Routine.MismatchPrefix + Hex),
          COFF::IMAGE_COMDAT_SELECT_LARGEST,
          Align(getKCFIOpenRoutineAlignment()));
      emitKCFIOpenRoutine(Routine, List, Open.Dynamic);
    }
    emitKCFIList(List, Type, Open.Imports);
  }
}
