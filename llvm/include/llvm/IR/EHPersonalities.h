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
#include <cstdint>
#include <optional>

namespace llvm {
class BasicBlock;
class CleanupPadInst;
class Function;
class Instruction;
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

/// Returns true if this personality uses scope-style EH IR instructions:
/// catchswitch, catchpad/ret, and cleanuppad/ret.
inline bool isScopedEHPersonality(EHPersonality Pers) {
  switch (Pers) {
  case EHPersonality::MSVC_CXX:
  case EHPersonality::MSVC_X86SEH:
  case EHPersonality::MSVC_TableSEH:
  case EHPersonality::CoreCLR:
  case EHPersonality::Wasm_CXX:
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

/// Whether \p F's cleanups are funclets under an Itanium-style personality:
/// on the NT-POSIX environment a cleanuppad is an outlined routine the
/// unwinder calls with the establisher frame, while a landingpad in the same
/// function is landed as usual. Such a function is prepared and laid out as a
/// funclet function without its personality being one.
LLVM_ABI bool usesNTPOSIXCleanupFunclets(const Function &F);

/// What a search does at the call sites an NT-POSIX cleanup funclet names,
/// carried as the cleanuppad's one `i8` argument. It is the site's own
/// clause, independent of how the cleanup exits, so no optimisation of the
/// funclet's body changes it: a front end writes it on every cleanuppad from
/// the ABI of the body the pad is in, and the inliner joins a call site's
/// clause into every top-level pad it inlines there.
enum class NTPOSIXPhaseOne : uint8_t {
  /// The site's cleanup, and a search passes the site.
  Pass = 0,
  /// The site's cleanup, then the empty filter: a search ends at the site.
  Boundary = 1,
  /// The empty filter alone: the abort funclet of a body that cannot unwind.
  Terminate = 2,
};

/// Whether a search passes a site with this clause.
inline bool passesNTPOSIXSearch(NTPOSIXPhaseOne Clause) {
  return Clause == NTPOSIXPhaseOne::Pass;
}

/// The clause a cleanuppad carries, or `std::nullopt` when its arguments are
/// not exactly one `i8` naming one.
LLVM_ABI std::optional<NTPOSIXPhaseOne>
getNTPOSIXPhaseOne(const CleanupPadInst &Pad);

/// Rewrites a cleanuppad's one argument to \p Clause.
LLVM_ABI void setNTPOSIXPhaseOne(CleanupPadInst &Pad, NTPOSIXPhaseOne Clause);

/// The clause of a pad inlined at a call site whose own clause is \p Site: a
/// site that ends the search ends it for every cleanup inlined there, and the
/// abort funclet stays one.
inline NTPOSIXPhaseOne joinNTPOSIXPhaseOne(NTPOSIXPhaseOne Inlined,
                                           NTPOSIXPhaseOne Site) {
  if (Inlined == NTPOSIXPhaseOne::Terminate || passesNTPOSIXSearch(Site))
    return Inlined;
  return NTPOSIXPhaseOne::Boundary;
}

/// The first instruction of \p F that breaks the NT-POSIX clause discipline,
/// or null: a cleanuppad carrying no clause; a `cleanupret` joining a pad a
/// search passes to one it does not, in either direction; a pad a search does
/// not pass resuming to the caller. Nested pads inherit their parent's
/// answer, since a raise inside a funclet is fatal before any search.
LLVM_ABI const Instruction *findNTPOSIXPhaseOneViolation(const Function &F);

typedef TinyPtrVector<BasicBlock *> ColorVector;

/// If an EH funclet personality is in use (see isFuncletEHPersonality),
/// this will recompute which blocks are in which funclet. It is possible that
/// some blocks are in multiple funclets. Consider this analysis to be
/// expensive.
LLVM_ABI DenseMap<BasicBlock *, ColorVector> colorEHFunclets(Function &F);

} // end namespace llvm

#endif // LLVM_IR_EHPERSONALITIES_H
