//===-- ModuleUtils.h - Functions to manipulate Modules ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This family of functions perform manipulations on Modules.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_MODULEUTILS_H
#define LLVM_TRANSFORMS_UTILS_MODULEUTILS_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MemoryBufferRef.h"
#include <utility> // for std::pair

namespace llvm {
template <typename T> class SmallVectorImpl;

template <typename T> class ArrayRef;
class CallBase;
class Module;
class Function;
class FunctionCallee;
class GlobalIFunc;
class GlobalValue;
class TargetTransformInfo;
class Constant;
class ConstantStruct;
class Instruction;
class Value;
class Type;
class raw_ostream;

/// Append F to the list of global ctors of module M with the given Priority.
/// This wraps the function in the appropriate structure and stores it along
/// side other global constructors. For details see
/// https://llvm.org/docs/LangRef.html#the-llvm-global-ctors-global-variable
LLVM_ABI void appendToGlobalCtors(Module &M, Function *F, int Priority,
                                  Constant *Data = nullptr);

/// Same as appendToGlobalCtors(), but for global dtors.
LLVM_ABI void appendToGlobalDtors(Module &M, Function *F, int Priority,
                                  Constant *Data = nullptr);

/// Apply 'Fn' to the list of global ctors of module M and replace contructor
/// record with the one returned by `Fn`. If `nullptr` was returned, the
/// corresponding constructor will be removed from the array. For details see
/// https://llvm.org/docs/LangRef.html#the-llvm-global-ctors-global-variable
using GlobalCtorTransformFn = llvm::function_ref<Constant *(Constant *)>;
LLVM_ABI void transformGlobalCtors(Module &M, const GlobalCtorTransformFn &Fn);
LLVM_ABI void transformGlobalDtors(Module &M, const GlobalCtorTransformFn &Fn);

/// Sets the KCFI type for the function. Used for compiler-generated functions
/// that are indirectly called in instrumented code.
LLVM_ABI void setKCFIType(Module &M, Function &F, StringRef MangledType);

LLVM_ABI FunctionCallee
declareSanitizerInitFunction(Module &M, StringRef InitName,
                             ArrayRef<Type *> InitArgTypes, bool Weak = false);

/// Creates sanitizer constructor function.
/// \return Returns pointer to constructor.
LLVM_ABI Function *createSanitizerCtor(Module &M, StringRef CtorName);

/// Creates sanitizer constructor function, and calls sanitizer's init
/// function from it.
/// \return Returns pair of pointers to constructor, and init functions
/// respectively.
LLVM_ABI std::pair<Function *, FunctionCallee>
createSanitizerCtorAndInitFunctions(Module &M, StringRef CtorName,
                                    StringRef InitName,
                                    ArrayRef<Type *> InitArgTypes,
                                    ArrayRef<Value *> InitArgs,
                                    StringRef VersionCheckName = StringRef(),
                                    bool Weak = false);

/// Creates sanitizer constructor function lazily. If a constructor and init
/// function already exist, this function returns it. Otherwise it calls \c
/// createSanitizerCtorAndInitFunctions. The FunctionsCreatedCallback is invoked
/// in that case, passing the new Ctor and Init function.
///
/// \return Returns pair of pointers to constructor, and init functions
/// respectively.
LLVM_ABI std::pair<Function *, FunctionCallee>
getOrCreateSanitizerCtorAndInitFunctions(
    Module &M, StringRef CtorName, StringRef InitName,
    ArrayRef<Type *> InitArgTypes, ArrayRef<Value *> InitArgs,
    function_ref<void(Function *, FunctionCallee)> FunctionsCreatedCallback,
    StringRef VersionCheckName = StringRef(), bool Weak = false);

/// Rename all the anon globals in the module using a hash computed from
/// the list of public globals in the module.
LLVM_ABI bool nameUnamedGlobals(Module &M);

/// Adds global values to the llvm.used list.
LLVM_ABI void appendToUsed(Module &M, ArrayRef<GlobalValue *> Values);

/// Adds global values to the llvm.compiler.used list.
LLVM_ABI void appendToCompilerUsed(Module &M, ArrayRef<GlobalValue *> Values);

/// Removes global values from the llvm.used and llvm.compiler.used arrays. \p
/// ShouldRemove should return true for any initializer field that should not be
/// included in the replacement global.
LLVM_ABI void removeFromUsedLists(Module &M,
                                  function_ref<bool(Constant *)> ShouldRemove);

/// Filter out potentially dead comdat functions where other entries keep the
/// entire comdat group alive.
///
/// This is designed for cases where functions appear to become dead but remain
/// alive due to other live entries in their comdat group.
///
/// The \p DeadComdatFunctions container should only have pointers to
/// `Function`s which are members of a comdat group and are believed to be
/// dead.
///
/// After this routine finishes, the only remaining `Function`s in \p
/// DeadComdatFunctions are those where every member of the comdat is listed
/// and thus removing them is safe (provided *all* are removed).
LLVM_ABI void
filterDeadComdatFunctions(SmallVectorImpl<Function *> &DeadComdatFunctions);

/// Produce a unique identifier for this module by taking the MD5 sum of
/// the names of the module's strong external symbols that are not comdat
/// members.
///
/// This identifier is normally guaranteed to be unique, or the program would
/// fail to link due to multiply defined symbols.
///
/// If the module has no strong external symbols (such a module may still have a
/// semantic effect if it performs global initialization), we cannot produce a
/// unique identifier for this module, so we return the empty string.
LLVM_ABI std::string getUniqueModuleId(Module *M);

/// Embed the memory buffer \p Buf into the module \p M as a global using the
/// specified section name. Also provide a metadata entry to identify it in the
/// module using the same section name. If \p SectionExclude is true !exclude
/// is applied to the global in order to apply necessary linkage flags to
/// exclude the section from a link. If false, apply !metadata_section_kind
/// which results in no additional section linkage flags.
LLVM_ABI GlobalVariable *embedBufferInModule(Module &M, MemoryBufferRef Buf,
                                             StringRef SectionName,
                                             Align Alignment = Align(1),
                                             bool SectionExclude = true);

/// Lower all calls to ifuncs by replacing uses with indirect calls loaded out
/// of a global table initialized in a global constructor. This will introduce
/// one constructor function and adds it to llvm.global_ctors. The constructor
/// will call the resolver function once for each ifunc.
///
/// Leaves any unhandled constant initializer uses as-is.
///
/// If \p IFuncsToLower is empty, all ifuncs in the module will be lowered.
/// If \p IFuncsToLower is non-empty, only the selected ifuncs will be lowered.
///
/// The processed ifuncs without remaining users will be removed from the
/// module.
LLVM_ABI bool
lowerGlobalIFuncUsersAsGlobalCtor(Module &M,
                                  ArrayRef<GlobalIFunc *> IFuncsToLower = {});

/// Collects in \p Versions the functions that \p Resolver, the resolver of a
/// multiversioned function's ifunc, may return. Returns false if a returned
/// value is anything but a multiversioned function reached through selects
/// and phis.
LLVM_ABI bool
collectFunctionVersions(Function &Resolver,
                        SmallVectorImpl<Function *> &Versions,
                        function_ref<TargetTransformInfo &(Function &)> GetTTI);

/// A multiversioned function whose body Windows selects as it maps the image.
struct FunctionOverride {
  /// The version every processor the target supports can run, which runs
  /// where Windows selects none.
  Function *Default = nullptr;
  /// The other versions, highest priority first, each with the numbers of the
  /// capabilities whose presence together selects it.
  SmallVector<std::pair<Function *, SmallVector<uint32_t, 2>>, 4> Candidates;
};

/// Replaces each declaration of a guard in \p M, which names the thread-local
/// variable whose record holds it, with the guard that the variable's record
/// states, where \p M defines the variable: a module that links a unit that
/// only declares a variable with the unit that defines it reads one guard.
LLVM_ABI void resolveThreadLocalGuards(Module &M);

/// Whether Windows can select the body of the multiversioned function \p GI,
/// and if so its versions in \p FO: those its resolver returns, ranked as
/// GlobalOpt ranks them. Otherwise writes the reason to \p Why if given.
/// \p GetCapabilities adds the numbers that test a version's features beyond
/// those every processor the target supports has, and returns a feature that
/// has none, or an empty string.
LLVM_ABI bool collectFunctionOverride(
    const GlobalIFunc &GI,
    function_ref<TargetTransformInfo &(Function &)> GetTTI,
    function_ref<StringRef(const Function &, SmallVectorImpl<uint32_t> &)>
        GetCapabilities,
    FunctionOverride &FO, raw_ostream *Why = nullptr);

/// Whether \p CB's callee reads the thread pointer, so that a caller holding
/// it saves a derivation by passing it: a definition not inferred
/// "thread-pointer-unused", or a declaration whose "thread-pointer-argument"
/// is "read" that may touch global memory.
LLVM_ABI bool callsThreadPointerReader(const CallBase &CB);

/// Whether \p CB, made by a function that holds a thread pointer, passes it:
/// a direct call of a function passed one that may read it, one that touches
/// no global memory never does. A function's address is its plain entry, so a
/// call through a pointer passes one only under a KCFI check, which finds the
/// target's thread-pointer entry from its type prefix, and without Control
/// Flow Guard checks, which accept no such entry.
LLVM_ABI bool passesThreadPointer(const CallBase &CB);

/// Whether \p I needs its function's thread pointer whatever its callees do:
/// it reads it, or calls a variadic ifunc, which has no plain entry.
LLVM_ABI bool needsThreadPointer(const Instruction &I);

/// Whether \p F holds a thread pointer to pass on: one passed it that is not
/// "thread-pointer-unused", one that comes by its own in a way that never
/// adopts or has none, or one that derives and needs it or calls a reader.
LLVM_ABI bool holdsThreadPointer(const Function &F);

/// Makes each address of a function of \p M that is passed its thread pointer
/// its plain entry, which code holding a thread pointer and code holding none
/// call alike: one address everywhere, in vtables and pointers as in what
/// foreign code is given. A resolver's version, which its ifunc hands its
/// thread pointer, and llvm.used and llvm.compiler.used keep the function.
LLVM_ABI void takeThreadPointerPlainEntries(Module &M);

/// Returns the plain entry of \p GV, a function passed its thread pointer or
/// an alias of one: \p GV's symbol with no "$tp", declared where \p GV is.
LLVM_ABI GlobalValue *getThreadPointerPlainEntry(GlobalValue &GV);

/// Gives each function of \p M that carries "thread-pointer-argument", and
/// that code passing none may call, its plain entry, the entry of its name
/// for such callers: every one that is not local, and a local one that a
/// function holding no thread pointer calls or whose address is taken. The
/// plain entry of a function that holds none is an alias of it; any other's is
/// a function of its type passed none, which calls it, or for a variadic
/// function a second body, since no call forwards its arguments. An ifunc
/// whose resolver carries the mark has the plain entry, which calls the
/// ifunc. The plain entry of a function whose "thread-pointer-plain-entry" is
/// "refusal" carries "thread-pointer-refusal". An alias names the entry of
/// its aliasee of its own kind, and a weak external's two entries fall back
/// to its alternate's.
LLVM_ABI void createThreadPointerPlainEntries(Module &M);

} // End llvm namespace

#endif // LLVM_TRANSFORMS_UTILS_MODULEUTILS_H
