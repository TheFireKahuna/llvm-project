//===-- CallTargetRestrictionPlugin.cpp - Restrict JIT'd call targets -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/CallTargetRestrictionPlugin.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/LookupAndApply.h"
#include "llvm/ExecutionEngine/Orc/Shared/OrcRTBridge.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"

using namespace llvm::jitlink;

namespace llvm::orc {

Expected<std::unique_ptr<CallTargetRestrictionPlugin>>
CallTargetRestrictionPlugin::Create(ExecutionSession &ES) {
  ExecutorAddr RestrictCallTargets;
  if (auto Err = lookupAndApply(
          ES.getBootstrapJITDylib(),
          {recordAddr(rt::RestrictCallTargetsAllocActionName,
                      &RestrictCallTargets,
                      SymbolLookupFlags::WeaklyReferencedSymbol)}))
    return std::move(Err);
  if (!RestrictCallTargets)
    return nullptr;
  return std::make_unique<CallTargetRestrictionPlugin>(RestrictCallTargets);
}

// Returns true if the graph's object was built with Control Flow Guard, and so
// lists in .gfids$y the functions whose address it takes.
static bool hasGuardCF(LinkGraph &G) {
  return llvm::any_of(G.absolute_symbols(), [](Symbol *Sym) {
    return Sym->hasName() && *Sym->getName() == "@feat.00" &&
           (Sym->getAddress().getValue() & COFF::Feat00Flags::GuardCF);
  });
}

void CallTargetRestrictionPlugin::modifyPassConfig(
    MaterializationResponsibility &MR, LinkGraph &LG,
    PassConfiguration &PassConfig) {
  // Collect the functions .gfids$y names before dead-stripping can remove it;
  // only those that stay live become entries.
  std::optional<DenseSet<Symbol *>> GFIDs;
  if (hasGuardCF(LG)) {
    GFIDs.emplace();
    if (Section *Sec = LG.findSectionByName(".gfids$y"))
      for (Block *B : Sec->blocks())
        for (Edge &E : B->edges())
          GFIDs->insert(&E.getTarget());
  }

  PassConfig.PostFixupPasses.push_back([this, GFIDs = std::move(GFIDs)](
                                           LinkGraph &G) -> Error {
    std::vector<ExecutorAddrRange> Code;
    std::vector<ExecutorAddr> Entries;
    for (Section &Sec : G.sections()) {
      if ((Sec.getMemProt() & MemProt::Exec) != MemProt::None &&
          !Sec.blocks().empty()) {
        Code.push_back(SectionRange(Sec).getRange());
        for (Symbol *Sym : Sec.symbols())
          if (Sym->getScope() != Scope::Local ||
              (GFIDs && GFIDs->contains(Sym)))
            Entries.push_back(Sym->getAddress());
      }
      if (GFIDs ||
          !object::COFFObjectFile::isGuardCFAddressTakingSection(Sec.getName()))
        continue;
      for (Block *B : Sec.blocks())
        for (Edge &E : B->edges())
          if (E.getKind() != Edge::KeepAlive && E.getTarget().isDefined() &&
              E.getTarget().isCallable())
            Entries.push_back(E.getTarget().getAddress());
    }
    if (Code.empty())
      return Error::success();
    llvm::sort(Entries);
    using namespace shared;
    G.allocActions().push_back(
        {cantFail(WrapperFunctionCall::Create<
                  SPSArgList<SPSSequence<SPSExecutorAddrRange>,
                             SPSSequence<SPSExecutorAddr>>>(RestrictCallTargets,
                                                            Code, Entries)),
         {}});
    return Error::success();
  });
}

Error getCallTargets(const object::COFFObjectFile &Obj,
                     const RuntimeDyld::LoadedObjectInfo &L,
                     SmallVectorImpl<sys::MemoryBlock> &Code,
                     std::vector<uintptr_t> &Entries) {
  bool GuardCF = false;
  for (const object::SymbolRef &Sym : Obj.symbols()) {
    object::COFFSymbolRef CSym = Obj.getCOFFSymbol(Sym);
    if (!CSym.isAbsolute())
      continue;
    Expected<StringRef> Name = Obj.getSymbolName(CSym);
    if (!Name)
      return Name.takeError();
    if (*Name == "@feat.00")
      GuardCF = CSym.getValue() & COFF::Feat00Flags::GuardCF;
  }

  // The indices of the functions whose address the object takes: those its
  // .gfids$y names if it was built with Control Flow Guard, otherwise those
  // its relocations name.
  DenseSet<uint32_t> Taken;
  for (const object::SectionRef &Sec : Obj.sections()) {
    Expected<StringRef> Name = Sec.getName();
    if (!Name)
      return Name.takeError();
    if (GuardCF && *Name == ".gfids$y") {
      Expected<StringRef> Data = Sec.getContents();
      if (!Data)
        return Data.takeError();
      for (size_t I = 0; I + 4 <= Data->size(); I += 4)
        Taken.insert(support::endian::read32le(Data->data() + I));
    }
    uint64_t Addr = L.getSectionLoadAddress(Sec);
    if (!Addr)
      continue;
    if (Sec.isText())
      Code.push_back({reinterpret_cast<void *>(Addr),
                      size_t(L.getSectionAllocationSize(Sec))});
    if (GuardCF || !Obj.isGuardCFAddressTakingSection(*Name))
      continue;
    for (const object::RelocationRef &R : Sec.relocations()) {
      object::COFFSymbolRef Target = Obj.getCOFFSymbol(*R.getSymbol());
      if (Obj.isGuardCFAddressTakingRelocation(R.getType()) &&
          Target.getComplexType() == COFF::IMAGE_SYM_DTYPE_FUNCTION)
        Taken.insert(Obj.getSymbolIndex(Target));
    }
  }

  for (const object::SymbolRef &Sym : Obj.symbols()) {
    object::COFFSymbolRef CSym = Obj.getCOFFSymbol(Sym);
    if (!CSym.isExternal() && !Taken.contains(Obj.getSymbolIndex(CSym)))
      continue;
    Expected<object::section_iterator> Sec = Sym.getSection();
    if (!Sec)
      return Sec.takeError();
    if (*Sec != Obj.section_end() && (*Sec)->isText())
      if (uint64_t Addr = L.getSectionLoadAddress(**Sec))
        Entries.push_back(Addr + CSym.getValue());
  }
  llvm::sort(Entries);
  return Error::success();
}

} // namespace llvm::orc
