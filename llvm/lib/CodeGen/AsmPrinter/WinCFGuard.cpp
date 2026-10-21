//===-- CodeGen/AsmPrinter/WinCFGuard.cpp - Control Flow Guard Impl ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains support for writing the metadata for Windows Control Flow
// Guard, including address-taken functions and valid longjmp targets.
//
//===----------------------------------------------------------------------===//

#include "WinCFGuard.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCStreamer.h"

#include <vector>

using namespace llvm;

WinCFGuard::WinCFGuard(AsmPrinter *A) : Asm(A) {}

WinCFGuard::~WinCFGuard() = default;

void WinCFGuard::endFunction(const MachineFunction *MF) {

  // Skip functions without any longjmp targets.
  if (MF->getLongjmpTargets().empty())
    return;

  // Copy the function's longjmp targets to a module-level list.
  llvm::append_range(LongjmpTargets, MF->getLongjmpTargets());
}

/// Returns true if this function's address is escaped in a way that might make
/// it an indirect call target. Function::hasAddressTaken gives different
/// results when a function is called directly with a function prototype
/// mismatch, which requires a cast.
// Whether C is an entry of llvm.global_ctors or llvm.global_dtors whose key
// is not defined in the module, which the AsmPrinter does not emit.
static bool isDroppedStructor(const Constant *C) {
  const auto *Entry = dyn_cast<ConstantStruct>(C);
  if (!Entry || Entry->getNumOperands() != 3)
    return false;
  const auto *Key =
      dyn_cast<GlobalValue>(Entry->getOperand(2)->stripPointerCasts());
  if (!Key || !Key->isDeclarationForLinker())
    return false;
  return llvm::all_of(Entry->users(), [](const User *List) {
    return llvm::all_of(List->users(), [](const User *U) {
      const auto *GV = dyn_cast<GlobalVariable>(U);
      return GV && (GV->getName() == "llvm.global_ctors" ||
                    GV->getName() == "llvm.global_dtors");
    });
  });
}

// Whether F is only the resolver of indirect functions that Windows Itanium
// and NT-POSIX lower to a cell, which the image's start-up writes with what F
// returns, calling F from read-only data with no check.
static bool isCellResolver(const Function &F) {
  return F.getParent()
             ->getTargetTriple()
             .isWindowsItaniumOrNTPOSIXEnvironment() &&
         !F.use_empty() && llvm::all_of(F.uses(), [](const Use &U) {
           auto *GI = dyn_cast<GlobalIFunc>(U.getUser());
           return GI && GI->getResolver() == U.get();
         });
}

// Whether the value of I, an instruction of a cell's resolver, only flows to
// what the resolver returns, through selects and phis.
static bool isOnlyReturned(const Instruction *I) {
  SmallVector<const Instruction *, 4> Work{I};
  SmallPtrSet<const Instruction *, 4> Seen;
  while (!Work.empty()) {
    const Instruction *V = Work.pop_back_val();
    if (isa<ReturnInst>(V))
      continue;
    if (!isa<SelectInst, PHINode>(V) || !Seen.insert(V).second)
      return isa<SelectInst, PHINode>(V);
    for (const User *U : V->users())
      Work.push_back(cast<Instruction>(U));
  }
  return true;
}

static bool isPossibleIndirectCallTarget(const GlobalValue *GV) {
  SmallVector<const Value *, 4> Users{GV};
  while (!Users.empty()) {
    const Value *FnOrCast = Users.pop_back_val();
    for (const Use &U : FnOrCast->uses()) {
      const User *FnUser = U.getUser();
      if (const auto *Call = dyn_cast<CallBase>(FnUser)) {
        if ((!Call->isCallee(&U) || U.get() != GV) &&
            !Call->getFunction()->getName().ends_with("$exit_thunk")) {
          // Passing a function pointer to a call may lead to an indirect
          // call. As an exception, ignore ARM64EC exit thunks.
          return true;
        }
      } else if (auto *I = dyn_cast<Instruction>(FnUser);
                 I && isCellResolver(*I->getFunction()) &&
                 (isa<ReturnInst>(I) ||
                  (isa<SelectInst, PHINode>(I) && isOnlyReturned(I) &&
                   !(isa<SelectInst>(I) &&
                     cast<SelectInst>(I)->getCondition() == U.get())))) {
        // What a cell's resolver returns is reached only through the cell.
        continue;
      } else if (isa<Instruction>(FnUser)) {
        // Consider any other instruction to be an escape. This has some weird
        // consequences like no-op intrinsics being an escape or a store *to* a
        // function address being an escape.
        return true;
      } else if (isa<GlobalAlias>(FnUser)) {
        // If the function is used via the alias, it's really the alias that's
        // a possible call target. See "Consider aliases" in endModule().
        continue;
      } else if (const auto *GI = dyn_cast<GlobalIFunc>(FnUser);
                 GI && GI->getResolverFunction() &&
                 isCellResolver(*GI->getResolverFunction())) {
        // The start-up calls a cell's resolver, not a pointer.
        continue;
      } else if (const auto *G = dyn_cast<GlobalValue>(FnUser)) {
        // Ignore llvm.arm64ec.symbolmap; it doesn't lower to an actual address.
        if (G->getName() == "llvm.arm64ec.symbolmap")
          continue;
        // A personality routine is named by the function's unwind data, and
        // the system calls it directly, without an indirect-call check.
        if (const auto *F = dyn_cast<Function>(G);
            F && F->hasPersonalityFn() && F->getPersonalityFn() == U.get())
          continue;
        // Globals (for example, vtables) are escapes.
        return true;
      } else if (isa<Constant>(FnUser) &&
                 isDroppedStructor(cast<Constant>(FnUser))) {
        // An initializer or finalizer whose key another module provides, as
        // LTO leaves one of a COMDAT that another object prevails for, is not
        // emitted.
        continue;
      } else if (isa<Constant>(FnUser)) {
        // Constants which aren't a global are intermediate values; recursively
        // analyze the users to see if they actually escape.
        Users.push_back(FnUser);
      }
    }
  }
  return false;
}

MCSymbol *WinCFGuard::lookupImpSymbol(const MCSymbol *Sym) {
  if (Sym->getName().starts_with("__imp_"))
    return nullptr;
  return Asm->OutContext.lookupSymbol(Twine("__imp_") + Sym->getName());
}

void WinCFGuard::endModule() {
  const Module *M = Asm->MMI->getModule();
  // On Windows Itanium and NT-POSIX a target may load the address of a
  // function that is not dllimport from its import pointer too.
  bool ImportsUndeclared =
      M->getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment();
  std::vector<const MCSymbol *> GFIDsEntries;
  std::vector<const MCSymbol *> GIATsEntries;
  std::vector<const MCSymbol *> SuppressedTargets;
  for (const Function &F : *M) {
    if (!F.isDeclaration() && F.hasFnAttribute("guard_suppress"))
      SuppressedTargets.push_back(Asm->getSymbol(&F));
    if (isPossibleIndirectCallTarget(&F)) {
      // If F is a dllimport and has an "__imp_" symbol already defined, add the
      // "__imp_" symbol to the .giats section.
      if (F.hasDLLImportStorageClass() ||
          (ImportsUndeclared && F.isDeclarationForLinker())) {
        if (MCSymbol *impSym = lookupImpSymbol(Asm->getSymbol(&F))) {
          GIATsEntries.push_back(impSym);
        }
      }
      // Add the function's symbol to the .gfids section.
      // Note: For dllimport functions, MSVC sometimes does not add this symbol
      // to the .gfids section, but only adds the corresponding "__imp_" symbol
      // to the .giats section. Here we always add the symbol to the .gfids
      // section, since this does not introduce security risks.
      GFIDsEntries.push_back(Asm->getSymbol(&F));
    }
  }

  // An indirect function lowered to a stub is a function at the stub.
  for (const GlobalIFunc &GI : M->ifuncs())
    if (M->getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment() &&
        isPossibleIndirectCallTarget(&GI))
      GFIDsEntries.push_back(Asm->getSymbol(&GI));

  for (const GlobalAlias &GA : M->aliases()) {
    // Consider aliases to functions as possible call targets.
    const GlobalObject *Aliasee = GA.getAliaseeObject();
    if (Aliasee && isa<Function>(Aliasee) && isPossibleIndirectCallTarget(&GA))
      GFIDsEntries.push_back(Asm->getSymbol(&GA));
  }

  if (GFIDsEntries.empty() && GIATsEntries.empty() && LongjmpTargets.empty() &&
      SuppressedTargets.empty())
    return;

  // Name each suppressed target to the linker, as MSVC does, which lists it in
  // the function table marked as not a valid call target.
  auto &OS = *Asm->OutStreamer;
  if (!SuppressedTargets.empty()) {
    const Triple &TT = M->getTargetTriple();
    StringRef Flag = TT.isWindowsGNUEnvironment() ||
                             TT.isWindowsCygwinEnvironment()
                         ? " -guardsym:"
                         : " /GUARDSYM:";
    OS.switchSection(Asm->OutContext.getObjectFileInfo()->getDrectveSection());
    for (const MCSymbol *S : SuppressedTargets)
      OS.emitBytes((Flag + S->getName() + ",S").str());
  }

  // Emit the symbol index of each GFIDs entry to form the .gfids section.
  OS.switchSection(Asm->OutContext.getObjectFileInfo()->getGFIDsSection());
  for (const MCSymbol *S : GFIDsEntries)
    OS.emitCOFFSymbolIndex(S);

  // Emit the symbol index of each GIATs entry to form the .giats section.
  OS.switchSection(Asm->OutContext.getObjectFileInfo()->getGIATsSection());
  for (const MCSymbol *S : GIATsEntries) {
    OS.emitCOFFSymbolIndex(S);
  }

  // Emit the symbol index of each longjmp target to form the .gljmp section.
  OS.switchSection(Asm->OutContext.getObjectFileInfo()->getGLJMPSection());
  for (const MCSymbol *S : LongjmpTargets) {
    OS.emitCOFFSymbolIndex(S);
  }
}
