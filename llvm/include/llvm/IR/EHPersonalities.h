//===- EHPersonalities.h - Compute EH-related information -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_EHPERSONALITIES_H
#define LLVM_IR_EHPERSONALITIES_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Compiler.h"

namespace llvm {
class BasicBlock;
class Function;
class Triple;
class Value;

enum class EHPersonality {
  Unknown,
  GNU_Ada,
  GNU_C,
  GNU_C_SjLj,
  GNU_CXX,
  GNU_CXX_SjLj,
  GNU_ObjC,
  MSVC_X86SEH,
  MSVC_TableSEH,
  MSVC_CXX,
  CoreCLR,
  Rust,
  Wasm_CXX,
  XL_CXX,
  ZOS_CXX,
  Wasm_D,
  Itanium_SEH
};

/// See if the given exception handling personality function is one
/// that we understand.  If so, return a description of it; otherwise return
/// Unknown.
LLVM_ABI EHPersonality classifyEHPersonality(const Value *Pers);

LLVM_ABI StringRef getEHPersonalityName(EHPersonality Pers);

LLVM_ABI EHPersonality getDefaultEHPersonality(const Triple &T);

/// Returns true if this personality function catches asynchronous
/// exceptions.
inline bool isAsynchronousEHPersonality(EHPersonality Pers) {
  // The two SEH personality functions can catch asynch exceptions. We assume
  // unknown personalities don't catch asynch exceptions.
  switch (Pers) {
  case EHPersonality::MSVC_X86SEH:
  case EHPersonality::MSVC_TableSEH:
    return true;
  default:
    return false;
  }
  llvm_unreachable("invalid enum");
}

/// Returns true if this is a personality function that invokes
/// handler funclets (which must return to it).
inline bool isFuncletEHPersonality(EHPersonality Pers) {
  switch (Pers) {
  case EHPersonality::MSVC_CXX:
  case EHPersonality::MSVC_X86SEH:
  case EHPersonality::MSVC_TableSEH:
  case EHPersonality::CoreCLR:
    return true;
  default:
    return false;
  }
  llvm_unreachable("invalid enum");
}

/// Returns true if this personality's catchpads are landed: its unwinder
/// restores the function's frame and continues at the pad, which is code of
/// the function and no funclet, and whose catchret is a branch.
inline bool hasLandedCatchPads(EHPersonality Pers) {
  return isAsynchronousEHPersonality(Pers) ||
         Pers == EHPersonality::Itanium_SEH;
}

/// Returns true if this personality uses scope-style EH IR instructions:
/// catchswitch, catchpad/ret, and cleanuppad/ret.
inline bool isScopedEHPersonality(EHPersonality Pers) {
  switch (Pers) {
  case EHPersonality::MSVC_CXX:
  case EHPersonality::MSVC_X86SEH:
  case EHPersonality::MSVC_TableSEH:
  case EHPersonality::CoreCLR:
  case EHPersonality::Wasm_CXX:
  case EHPersonality::Wasm_D:
  case EHPersonality::Itanium_SEH:
    return true;
  default:
    return false;
  }
  llvm_unreachable("invalid enum");
}

/// Return true if this personality may be safely removed if there
/// are no invoke instructions remaining in the current function.
inline bool isNoOpWithoutInvoke(EHPersonality Pers) {
  switch (Pers) {
  case EHPersonality::Unknown:
    return false;
  // All known personalities currently have this behavior
  default:
    return true;
  }
  llvm_unreachable("invalid enum");
}

LLVM_ABI bool canSimplifyInvokeNoUnwind(const Function *F);

/// Returns true if \p F names an Itanium-style personality routine on Windows
/// Itanium (EHPersonality::Itanium_SEH). Its cleanuppads are funclets that the
/// unwinder calls and nothing else enters. Its catchpads are landed: the
/// unwinder restores the function's frame and continues at the pad, which is
/// code of the function and no funclet, and whose catchret is a branch. The
/// argument of a catchpad is the type its handler takes, null for every type,
/// or a constant array of the types an exception specification allows.
LLVM_ABI bool usesCalledCleanups(const Function &F);

/// Returns true if \p F's exception tables cover every instruction of an EH
/// scope and not only its calls, so that an exception any instruction raises
/// reaches the scope's EH pad: \p F names the personality routine of
/// asynchronous exceptions on Windows Itanium. Invokes of llvm.seh.scope.begin
/// and llvm.seh.try.begin then open the scope of their unwind destination, and
/// invokes of llvm.seh.scope.end and llvm.seh.try.end close it.
LLVM_ABI bool usesAsyncEHTables(const Function &F);

/// Returns true if \p F's MSVC exception states cover every instruction of a
/// __try block or an object's lifetime and not only its calls (-EHa): its
/// module is compiled for asynchronous exceptions, or \p F opens a __try block
/// with llvm.seh.try.begin, which a front end emits only for them.
LLVM_ABI bool isAsynchEH(const Function &F);

typedef TinyPtrVector<BasicBlock *> ColorVector;

/// If an EH funclet personality is in use (see isFuncletEHPersonality),
/// this will recompute which blocks are in which funclet. It is possible that
/// some blocks are in multiple funclets. Consider this analysis to be
/// expensive.
LLVM_ABI DenseMap<BasicBlock *, ColorVector> colorEHFunclets(Function &F);

} // end namespace llvm

#endif // LLVM_IR_EHPERSONALITIES_H
