//===-- Mangler.cpp - Self-contained c/asm llvm name mangler --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Unified name mangler for assembly backends.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Mangler.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace {
enum ManglerPrefixTy {
  Default,      ///< Emit default string before each symbol.
  Private,      ///< Emit "private" prefix before each symbol.
  LinkerPrivate ///< Emit "linker private" prefix before each symbol.
};
}

static void getNameWithPrefixImpl(raw_ostream &OS, const Twine &GVName,
                                  ManglerPrefixTy PrefixTy,
                                  const DataLayout &DL, char Prefix) {
  SmallString<256> TmpData;
  StringRef Name = GVName.toStringRef(TmpData);
  assert(!Name.empty() && "getNameWithPrefix requires non-empty name");

  // No need to do anything special if the global has the special "do not
  // mangle" flag in the name.
  if (Name[0] == '\1') {
    OS << Name.substr(1);
    return;
  }

  if (DL.doNotMangleLeadingQuestionMark() && Name[0] == '?')
    Prefix = '\0';

  if (PrefixTy == Private)
    OS << DL.getInternalSymbolPrefix();
  else if (PrefixTy == LinkerPrivate)
    OS << DL.getLinkerPrivateGlobalPrefix();

  if (Prefix != '\0')
    OS << Prefix;

  // If this is a simple string that doesn't need escaping, just append it.
  OS << Name;
}

static void getNameWithPrefixImpl(raw_ostream &OS, const Twine &GVName,
                                  const DataLayout &DL,
                                  ManglerPrefixTy PrefixTy) {
  char Prefix = DL.getGlobalPrefix();
  return getNameWithPrefixImpl(OS, GVName, PrefixTy, DL, Prefix);
}

void Mangler::getNameWithPrefix(raw_ostream &OS, const Twine &GVName,
                                const DataLayout &DL) {
  return getNameWithPrefixImpl(OS, GVName, DL, Default);
}

void Mangler::getNameWithPrefix(SmallVectorImpl<char> &OutName,
                                const Twine &GVName, const DataLayout &DL) {
  raw_svector_ostream OS(OutName);
  char Prefix = DL.getGlobalPrefix();
  return getNameWithPrefixImpl(OS, GVName, Default, DL, Prefix);
}

static bool hasByteCountSuffix(CallingConv::ID CC) {
  switch (CC) {
  case CallingConv::X86_FastCall:
  case CallingConv::X86_StdCall:
  case CallingConv::X86_VectorCall:
    return true;
  default:
    return false;
  }
}

/// Microsoft fastcall and stdcall functions require a suffix on their name
/// indicating the number of words of arguments they take.
static void addByteCountSuffix(raw_ostream &OS, const Function *F,
                               const DataLayout &DL) {
  // Calculate arguments size total.
  unsigned ArgWords = 0;

  const unsigned PtrSize = DL.getPointerSize();

  for (const Argument &A : F->args()) {
    // For the purposes of the byte count suffix, structs returned by pointer
    // do not count as function arguments.
    if (A.hasStructRetAttr())
      continue;

    // 'Dereference' type in case of byval or inalloca parameter attribute.
    uint64_t AllocSize = A.hasPassPointeeByValueCopyAttr() ?
      A.getPassPointeeByValueCopySize(DL) :
      DL.getTypeAllocSize(A.getType());

    // Size should be aligned to pointer size.
    ArgWords += alignTo(AllocSize, PtrSize);
  }

  OS << '@' << ArgWords;
}

void Mangler::getNameWithPrefix(raw_ostream &OS, const GlobalValue *GV,
                                bool CannotUsePrivateLabel) const {
  ManglerPrefixTy PrefixTy = Default;
  assert(GV != nullptr && "Invalid Global Value");
  if (GV->hasPrivateLinkage()) {
    if (CannotUsePrivateLabel)
      PrefixTy = LinkerPrivate;
    else
      PrefixTy = Private;
  }

  const DataLayout &DL = GV->getDataLayout();
  if (!GV->hasName()) {
    // Get the ID for the global, assigning a new one if we haven't got one
    // already.
    unsigned &ID = AnonGlobalIDs[GV];
    if (ID == 0)
      ID = AnonGlobalIDs.size();

    // Must mangle the global into a unique ID.
    getNameWithPrefixImpl(OS, "__unnamed_" + Twine(ID), DL, PrefixTy);
    return;
  }

  StringRef Name = GV->getName();
  char Prefix = DL.getGlobalPrefix();

  // Mangle functions with Microsoft calling conventions specially.  Only do
  // this mangling for x86_64 vectorcall and 32-bit x86.
  const Function *MSFunc = dyn_cast_or_null<Function>(GV->getAliaseeObject());

  // Don't add byte count suffixes when '\01' or '?' are in the first
  // character.
  if (Name.starts_with("\01") ||
      (DL.doNotMangleLeadingQuestionMark() && Name.starts_with("?")))
    MSFunc = nullptr;

  CallingConv::ID CC =
      MSFunc ? MSFunc->getCallingConv() : (unsigned)CallingConv::C;
  if (!DL.hasMicrosoftFastStdCallMangling() &&
      CC != CallingConv::X86_VectorCall)
    MSFunc = nullptr;
  if (MSFunc) {
    if (CC == CallingConv::X86_FastCall)
      Prefix = '@'; // fastcall functions have an @ prefix instead of _.
    else if (CC == CallingConv::X86_VectorCall)
      Prefix = '\0'; // vectorcall functions have no prefix.
  }

  getNameWithPrefixImpl(OS, Name, PrefixTy, DL, Prefix);

  // A function that is passed its thread pointer says so in its symbol, so
  // that a reference which passes none never binds it.
  if (const auto *F = dyn_cast_or_null<Function>(GV->getAliaseeObject());
      F && !Name.starts_with("\01") &&
      F->hasFnAttribute("thread-pointer-argument"))
    OS << "$tp";

  if (!MSFunc)
    return;

  // If we are supposed to add a microsoft-style suffix for stdcall, fastcall,
  // or vectorcall, add it.  These functions have a suffix of @N where N is the
  // cumulative byte size of all of the parameters to the function in decimal.
  if (CC == CallingConv::X86_VectorCall)
    OS << '@'; // vectorcall functions use a double @ suffix.
  FunctionType *FT = MSFunc->getFunctionType();
  if (hasByteCountSuffix(CC) &&
      // "Pure" variadic functions do not receive @0 suffix.
      (!FT->isVarArg() || FT->getNumParams() == 0 ||
       (FT->getNumParams() == 1 && MSFunc->hasStructRetAttr())))
    addByteCountSuffix(OS, MSFunc, DL);
}

void Mangler::getNameWithPrefix(SmallVectorImpl<char> &OutName,
                                const GlobalValue *GV,
                                bool CannotUsePrivateLabel) const {
  raw_svector_ostream OS(OutName);
  getNameWithPrefix(OS, GV, CannotUsePrivateLabel);
}

// Check if the name needs quotes to be safe for the linker to interpret.
static bool canBeUnquotedInDirective(char C) {
  return isAlnum(C) || C == '_' || C == '@' || C == '#';
}

static bool canBeUnquotedInDirective(StringRef Name) {
  if (Name.empty())
    return false;

  // If any of the characters in the string is an unacceptable character, force
  // quotes.
  for (char C : Name) {
    if (!canBeUnquotedInDirective(C))
      return false;
  }

  return true;
}

void llvm::emitLinkerFlagsForGlobalCOFF(raw_ostream &OS, const GlobalValue *GV,
                                        const Triple &TT, Mangler &Mangler) {
  // A function declaration that names its alternate is a weak definition.
  const auto *F = dyn_cast<Function>(GV);
  if (GV->hasDLLExportStorageClass() &&
      (!GV->isDeclaration() || (F && F->hasFnAttribute("weak-alternate")))) {

    if (TT.isWindowsMSVCEnvironment() || TT.isUEFI())
      OS << " /EXPORT:";
    else
      OS << " -export:";

    bool NeedQuotes = GV->hasName() && !canBeUnquotedInDirective(GV->getName());
    if (NeedQuotes)
      OS << "\"";
    if (TT.isWindowsGNUEnvironment() || TT.isWindowsCygwinEnvironment()) {
      std::string Flag;
      raw_string_ostream FlagOS(Flag);
      Mangler.getNameWithPrefix(FlagOS, GV, false);
      if (Flag[0] == GV->getDataLayout().getGlobalPrefix())
        OS << Flag.substr(1);
      else
        OS << Flag;
    } else {
      Mangler.getNameWithPrefix(OS, GV, false);
    }
    if (TT.isWindowsArm64EC()) {
      // Use EXPORTAS for mangled ARM64EC symbols.
      // FIXME: During LTO, we're invoked prior to the EC lowering pass,
      // so symbols are not yet mangled. Emitting the unmangled name
      // typically functions correctly; the linker can resolve the export
      // with the demangled alias.
      if (std::optional<std::string> demangledName =
              getArm64ECDemangledFunctionName(GV->getName()))
        OS << ",EXPORTAS," << *demangledName;
    }
    if (NeedQuotes)
      OS << "\"";

    if (!GV->getValueType()->isFunctionTy()) {
      if (TT.isWindowsMSVCEnvironment() || TT.isUEFI())
        OS << ",DATA";
      else
        OS << ",data";
    }
  }
  if (GV->hasHiddenVisibility() && !GV->isDeclaration() && TT.isOSCygMing()) {

    OS << " -exclude-symbols:";

    bool NeedQuotes = GV->hasName() && !canBeUnquotedInDirective(GV->getName());
    if (NeedQuotes)
      OS << "\"";

    std::string Flag;
    raw_string_ostream FlagOS(Flag);
    Mangler.getNameWithPrefix(FlagOS, GV, false);
    if (Flag[0] == GV->getDataLayout().getGlobalPrefix())
      OS << Flag.substr(1);
    else
      OS << Flag;

    if (NeedQuotes)
      OS << "\"";
  }
}

const ConstantInt *llvm::getFunctionTypePrefixMarker(const GlobalObject &F) {
  const Module *M = F.getParent();
  if (!F.hasMetadata(LLVMContext::MD_kcfi_type) ||
      (M->getTargetTriple().isX86() && !M->getModuleFlag("kcfi")))
    return nullptr;
  return mdconst::extract_or_null<ConstantInt>(
      M->getModuleFlag("function-type-prefix"));
}

// Whether GV is a name of a vague entity: a definition that each unit that
// needs it emits, in a COMDAT that the link chooses among, and of which a
// process has one. So is every alias of one and whatever shares its COMDAT, a
// guard variable among them, which move with it. A weak definition without a
// COMDAT is a weak external, which the first copy in each image serves, as a
// C99 inline definition's body does. A function with any other linkage may
// differ from the definition that replaces it, so its calls could not stay
// with its image.
static bool isVagueEntity(const GlobalValue *GV) {
  const GlobalObject *GO = GV->getAliaseeObject();
  if (!GO || GV->hasLocalLinkage() || isa<GlobalIFunc>(GV))
    return false;
  if (GO != GV)
    return isVagueEntity(GO);
  const Comdat *C = GO->getComdat();
  if (!C)
    return false;
  if (C->getName() != GO->getName()) {
    const GlobalValue *Key = GO->getParent()->getNamedValue(C->getName());
    return Key && Key->getComdat() == C && isa<GlobalObject>(Key) &&
           isVagueEntity(Key);
  }
  if (!COFF::isVagueEntityName(GO->getName()))
    return false;
  if (isa<Function>(GO))
    return GO->hasLinkOnceODRLinkage() || GO->hasWeakODRLinkage();
  return GO->hasLinkOnceLinkage() || GO->hasWeakLinkage();
}

// Whether the unit takes the address of GV, a function or an alias of one: a
// use that is not the callee of a call. A module that is not materialized
// shows none of its uses, so each address counts as taken.
static bool isAddressTaken(const GlobalValue *GV) {
  return !GV->getParent()->isMaterialized() ||
         any_of(GV->uses(), [](const Use &U) {
           auto *Call = dyn_cast<CallBase>(U.getUser());
           return !Call || !Call->isCallee(&U);
         });
}

void llvm::emitLinkFactsForGlobalCOFF(
    const GlobalValue *GV, const Triple &TT,
    function_ref<void(uint64_t Tag, ArrayRef<LinkFactOperand> Operands)> Fact) {
  if (!TT.isWindowsItaniumOrNTPOSIXEnvironment())
    return;

  auto *Var = dyn_cast_or_null<GlobalVariable>(GV->getAliaseeObject());
  uint64_t State = GV->hasHiddenVisibility()      ? COFF::LinkFactHidden
                   : GV->hasProtectedVisibility() ? COFF::LinkFactProtected
                   : Var && Var->hasAttribute("gnu-unique")
                       ? COFF::LinkFactUnique
                       : COFF::LinkFactDefault;
  if (GV->hasExternalWeakLinkage() && State > COFF::LinkFactDefault)
    Fact(COFF::LinkFactVisibility, {State});
  if (GV->isDeclarationForLinker())
    return;

  if (auto *F = dyn_cast<GlobalObject>(GV);
      F && getFunctionTypePrefixMarker(*F))
    Fact(COFF::LinkFactTypePrefix,
         {F->hasMetadata("kcfi_vfn_type") ? COFF::LinkFactVfnType
                                          : COFF::LinkFactNoSecondWord});

  // A function that a program replaces for every image says so under each of
  // its names, as does a definition that replaces one. A default's own name
  // states its slot. Any other weak definition that another image may see is
  // replaced by its name.
  auto *Fn = dyn_cast_or_null<Function>(GV->getAliaseeObject());
  if (const MDNode *Slots = Fn ? Fn->getMetadata("overridable") : nullptr) {
    SmallVector<LinkFactOperand, 3> Operands;
    if (Fn == GV)
      for (const MDOperand &Slot : Slots->operands())
        Operands.push_back(
            mdconst::extract<ConstantInt>(Slot)->getZExtValue());
    Fact(COFF::LinkFactVisibility, {State});
    Fact(COFF::LinkFactOverridable, Operands);
  } else if (Fn == GV && Fn->hasWeakAnyLinkage() &&
             State == COFF::LinkFactDefault) {
    Fact(COFF::LinkFactOverridableByName, {});
  }

  // A thread-local variable that has a record states its guard. One that
  // lost its guard states none, which fails a link that needs the record.
  // Images share a vague one by its record, so the record is stated before
  // what is stated of a vague entity.
  if (const MDNode *Record =
          Var && Var == GV ? Var->getMetadata("tls_record") : nullptr) {
    if (auto *Guard =
            mdconst::dyn_extract_or_null<GlobalValue>(Record->getOperand(0)))
      Fact(COFF::LinkFactTlsRecord, {Guard});
    else
      Fact(COFF::LinkFactTlsRecord, {});
  }

  // Only an observable entity is stated: data, and a function whose address
  // is significant and taken; a call does not observe it. Every unit that
  // takes a linkonce_odr function's address has a copy that states it, but a
  // weak_odr one, an explicit instantiation definition, may be taken where
  // only its declaration is seen, exported or not, so only unnamed_addr,
  // which holds in every unit, leaves it out. A hidden function states
  // nothing, each image keeping its own.
  if (!isVagueEntity(GV) ||
      (!Var && (State == COFF::LinkFactHidden || GV->hasGlobalUnnamedAddr() ||
                (!GV->hasWeakODRLinkage() &&
                 (GV->hasAtLeastLocalUnnamedAddr() || !isAddressTaken(GV))))))
    return;
  Fact(COFF::LinkFactVisibility, {State});
  if (!Var) {
    Fact(COFF::LinkFactAddressSignificant, {});
    return;
  }
  StringRef Dynamic = Var->getAttribute("dynamic-init").getValueAsString();
  Fact(COFF::LinkFactInit,
       {Dynamic == "guarded"        ? COFF::LinkFactDynamicGuarded
        : Dynamic == "unordered"    ? COFF::LinkFactDynamicUnordered
        : Dynamic == "thread-local" ? COFF::LinkFactTLSDynamic
        : Var->isConstant() && Var->hasDefinitiveInitializer()
            ? COFF::LinkFactConstantImmutable
            : COFF::LinkFactConstantMutable});
  if (Var->hasAttribute("type-info"))
    Fact(COFF::LinkFactDescriptor, {});
}

void llvm::emitLinkerFlagsForUsedCOFF(raw_ostream &OS, const GlobalValue *GV,
                                      const Triple &T, Mangler &M) {
  if (!T.isWindowsMSVCEnvironment() &&
      !T.isWindowsItaniumOrNTPOSIXEnvironment())
    return;

  OS << " /INCLUDE:";
  bool NeedQuotes = GV->hasName() && !canBeUnquotedInDirective(GV->getName());
  if (NeedQuotes)
    OS << "\"";
  M.getNameWithPrefix(OS, GV, false);
  if (NeedQuotes)
    OS << "\"";
}

std::optional<std::string> llvm::getArm64ECMangledFunctionName(StringRef Name) {
  assert(!Name.empty() &&
         "getArm64ECMangledFunctionName requires non-empty name");

  if (Name[0] != '?') {
    // For non-C++ symbols, prefix the name with "#" unless it's already
    // mangled.
    if (Name[0] == '#')
      return std::nullopt;
    return std::optional<std::string>(("#" + Name).str());
  }

  // If the name contains $$h, then it is already mangled.
  if (Name.contains("$$h"))
    return std::nullopt;

  // Handle MD5 mangled names, which use a slightly different rule from
  // other C++ manglings.
  //
  // A non-Arm64EC function:
  //
  // ??@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa@
  //
  // An Arm64EC function:
  //
  // ??@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa@$$h@
  if (Name.starts_with("??@") && Name.ends_with("@"))
    return (Name + "$$h@").str();

  // Ask the demangler where we should insert "$$h".
  auto InsertIdx = getArm64ECInsertionPointInMangledName(Name);
  if (!InsertIdx)
    return std::nullopt;

  return std::optional<std::string>(
      (Name.substr(0, *InsertIdx) + "$$h" + Name.substr(*InsertIdx)).str());
}

std::optional<std::string>
llvm::getArm64ECDemangledFunctionName(StringRef Name) {
  // For non-C++ names, drop the "#" prefix.
  if (Name[0] == '#')
    return std::optional<std::string>(Name.substr(1));
  if (Name[0] != '?')
    return std::nullopt;

  // MD5 mangled name; see comment in getArm64ECMangledFunctionName.
  if (Name.starts_with("??@") && Name.ends_with("@$$h@"))
    return Name.drop_back(4).str();

  // Drop the ARM64EC "$$h" tag.
  std::pair<StringRef, StringRef> Pair = Name.split("$$h");
  if (Pair.second.empty())
    return std::nullopt;
  return std::optional<std::string>((Pair.first + Pair.second).str());
}
