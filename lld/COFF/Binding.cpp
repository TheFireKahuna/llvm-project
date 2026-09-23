//===- Binding.cpp - Cross-image symbol ownership
//---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Binding.h"
#include "COFFLinkerContext.h"
#include "Symbols.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFFBinding.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::support::endian;

namespace lld::coff {

static bool sameDefinition(Symbol *a, Symbol *b) {
  if (a == b)
    return true;
  auto *left = dyn_cast<DefinedRegular>(a);
  auto *right = dyn_cast<DefinedRegular>(b);
  // Bitcode definitions share placeholder text/data chunks. Those placeholders
  // prove no address relationship between different IR symbols.
  return left && right && isa_and_nonnull<ObjFile>(left->getFile()) &&
         isa_and_nonnull<ObjFile>(right->getFile()) && left->data &&
         right->data && left->getChunk() == right->getChunk() &&
         left->getValue() == right->getValue();
}

Symbol *getBindingTarget(Symbol *requirement) {
  Symbol *target = requirement->getDefined();
  if (!target)
    return requirement;
  auto *def = dyn_cast<DefinedRegular>(target);
  if (def && def->data) {
    DefinedRegular *leader = def->getChunk()->sym;
    if (leader && sameDefinition(def, leader))
      return leader;
  }
  return target;
}

bool isReplaceableBinding(DefinedRegular *symbol) {
  // COFF marks the COMDAT key as a COMDAT symbol, but other labels in that
  // contribution are regular symbols. They share its selected storage and
  // lifetime. Bitcode placeholders, on the other hand, prove no such relation.
  return symbol->isCOMDAT || (symbol->data && isa<ObjFile>(symbol->getFile()) &&
                              symbol->getChunk()->isCOMDAT());
}

void SymbolTable::collectABIRequirements(
    InputFile *file, ArrayRef<ABIRequirement> requirements) {
  for (const ABIRequirement &requirement : requirements) {
    auto [it, inserted] =
        internedABIContracts.try_emplace(requirement.contract);
    if (inserted) {
      auto graph = abi::Contract::decode(requirement.contract);
      if (!graph)
        Fatal(ctx) << toString(file) << ": " << graph.takeError();
      it->second = make<ABIContract>(ABIContract{
          it->first(), saver().save(abi::contractDigest(it->first())),
          std::move(*graph), file});
    }
    ABIContract *contract = it->second;
    // The bytes came from this input's use or definition. A selected fallback
    // determines storage only; it never supplies a replacement requirement.
    Symbol *target = getBindingTarget(requirement.symbol);
    if (requirement.definition && target->getFile() == file)
      offeredABIContracts.try_emplace(target, contract);
    auto [selected, first] = abiContracts.try_emplace(target, contract);
    if (first || selected->second == contract)
      continue;
    ABIContract *previous = selected->second;
    bool previousSatisfied = previous->graph.isSatisfiedBy(contract->graph);
    bool currentSatisfied = contract->graph.isSatisfiedBy(previous->graph);
    if (!previousSatisfied && !currentSatisfied) {
      auto joined = previous->graph.mergeWith(contract->graph);
      if (!joined)
        Fatal(ctx) << toString(file) << ": incompatible physical ABI for "
                   << requirement.symbol->getName() << " (resolved to "
                   << target->getName() << "); previous requirement in "
                   << toString(previous->origin) << ": " << joined.takeError();
      auto [merged, inserted] =
          internedABIContracts.try_emplace(joined->encode());
      if (inserted)
        merged->second = make<ABIContract>(ABIContract{
            merged->first(), saver().save(abi::contractDigest(merged->first())),
            std::move(*joined), previous->origin});
      selected->second = merged->second;
    }
    if (previousSatisfied && !currentSatisfied)
      selected->second = contract;
  }
}

void SymbolTable::validateABIContracts() {
  // Rebuild the selected-node index after weak alias/native LTO resolution.
  // Original per-input evidence, including losing COMDATs, remains intact.
  abiContracts.clear();
  offeredABIContracts.clear();
  for (ObjFile *file : ctx.objFileInstances)
    if (&file->symtab == this)
      collectABIRequirements(file, file->getABIRequirements());
  for (BitcodeFile *file : bitcodeFileInstances)
    collectABIRequirements(file, file->getABIRequirements());
  for (const auto &[symbol, required] : abiContracts) {
    if (!isa<DefinedRegular>(symbol))
      continue;
    ABIContract *offered = offeredABIContracts.lookup(symbol);
    if (!offered)
      Fatal(ctx) << toString(required->origin)
                 << ": physical ABI requirement for " << symbol->getName()
                 << " selects a definition without an ABI "
                    "contract in "
                 << toString(symbol->getFile());
    if (!required->graph.isSatisfiedBy(offered->graph))
      Fatal(ctx) << toString(required->origin) << ": definition of "
                 << symbol->getName() << " in " << toString(offered->origin)
                 << " does not satisfy its physical ABI requirement";
  }
}

void SymbolTable::collectBindingEntities(
    InputFile *file, ArrayRef<BindingRequirement> requirements) {
  for (const BindingRequirement &requirement : requirements) {
    // Follow the selected fallback, never the unselected arm of a conditional
    // weak alias. Keep the input requirements intact for subsequent ABI checks.
    Symbol *target = getBindingTarget(requirement.symbol);
    BindingEntity *entity = bindingRequirements.lookup(requirement.symbol);
    if (entity && !sameDefinition(entity->symbol, target))
      Fatal(ctx)
          << toString(file) << ": canonical requirement "
          << requirement.symbol->getName()
          << " changed its selected definition after ownership was frozen";
    if (!entity)
      entity = bindingEntities.lookup(target);
    if (!entity) {
      entity = make<BindingEntity>();
      entity->symbol = target;
      entity->requirement = requirement.symbol;
      entity->identity = requirement.symbol->getName();
      entity->origin = file;
      entity->flags = requirement.flags;
      entity->contract = abiContracts.lookup(target);
      bindingEntities[target] = entity;
    } else {
      constexpr uint32_t kinds = BindingRTTI | BindingName;
      if ((entity->flags & kinds) != (requirement.flags & kinds))
        Fatal(ctx) << toString(file) << ": conflicting binding kinds for "
                   << requirement.symbol->getName() << " (resolved to "
                   << target->getName() << "); previous requirement in "
                   << toString(entity->origin);
      entity->flags |= requirement.flags;
    }
    bindingRequirements[requirement.symbol] = entity;
    if (!is_contained(entity->requirements, requirement.symbol))
      entity->requirements.push_back(requirement.symbol);
    // Prefer a requirement on the selected definition itself to a wrapper.
    // Otherwise choose a stable semantic spelling before ownership is frozen.
    // Native .weak fallback labels never displace an originating requirement.
    if (entity->owner == BindingEntity::Unresolved &&
        (requirement.symbol == target ||
         (entity->requirement != target &&
          requirement.symbol->getName() < entity->identity))) {
      entity->requirement = requirement.symbol;
      entity->identity = requirement.symbol->getName();
      entity->origin = file;
    }
  }
}

static bool compareEntities(const BindingEntity *a, const BindingEntity *b) {
  return a->identity < b->identity;
}

bool sameImportedTarget(DefinedImportData *a, DefinedImportData *b) {
  return a->getDLLName().equals_insensitive(b->getDLLName()) &&
         (a->file->hdr->getNameType() == IMPORT_ORDINAL) ==
             (b->file->hdr->getNameType() == IMPORT_ORDINAL) &&
         (a->file->hdr->getNameType() == IMPORT_ORDINAL
              ? a->getOrdinal() == b->getOrdinal()
              : a->getExternalName() == b->getExternalName());
}

static DefinedImportData *findBindingImport(SymbolTable &symtab,
                                            BindingEntity &entity) {
  DefinedImportData *selected = nullptr;
  for (Symbol *requirement : entity.requirements) {
    Symbol *target = requirement->getDefined();
    if (!target)
      target = requirement;
    for (Symbol *candidate : {entity.symbol, target, requirement}) {
      auto *import = dyn_cast<DefinedImportData>(candidate);
      if (!import)
        import = dyn_cast_or_null<DefinedImportData>(
            symtab.find(("__imp_" + candidate->getName()).str()));
      if (!import)
        continue;
      if (entity.contract)
        import->file->requireABIContract(entity.lookupName);
      if (selected && !sameImportedTarget(selected, import))
        Fatal(symtab.ctx) << toString(entity.origin)
                          << ": canonical requirement "
                          << requirement->getName() << " resolves to "
                          << entity.symbol->getName()
                          << " but selects conflicting providers "
                          << selected->getDLLName() << ":"
                          << selected->getExternalName() << " and "
                          << import->getDLLName() << ":"
                          << import->getExternalName();
      if (!selected || import->getName() < selected->getName())
        selected = import;
    }
  }
  return selected;
}

DefinedImportData *SymbolTable::getBindingImport(Symbol *symbol) const {
  BindingEntity *entity = bindingEntities.lookup(symbol);
  if (!entity)
    entity = bindingRequirements.lookup(symbol);
  if (!entity)
    entity = bindingEntities.lookup(getBindingTarget(symbol));
  if (entity)
    if (entity->owner != BindingEntity::Unresolved)
      return entity->owner == BindingEntity::Imported ||
                     entity->owner == BindingEntity::Generated
                 ? entity->import
                 : nullptr;
  return dyn_cast_or_null<DefinedImportData>(
      find(("__imp_" + symbol->getName()).str()));
}

bool SymbolTable::hasCanonicalBinding(Symbol *symbol) const {
  BindingEntity *entity = bindingRequirements.lookup(symbol);
  if (!entity)
    entity = bindingEntities.lookup(getBindingTarget(symbol));
  return entity && (entity->flags & BindingCanonical);
}

static void hashBindingNumber(SHA256 &hash, uint64_t value) {
  uint8_t size[8];
  write64le(size, value);
  hash.update(size);
}

static void hashBindingField(SHA256 &hash, StringRef value) {
  hashBindingNumber(hash, value.size());
  hash.update(value);
}

std::string SymbolTable::getBindingContextHash() const {
  if (bindingEntities.empty() && ctx.config.privateRTTI.empty()) {
    bool partitioned = false;
    for (ObjFile *file : ctx.objFileInstances)
      partitioned |= !file->partitionRoots.empty();
    for (BitcodeFile *file : bitcodeFileInstances)
      partitioned |= !file->partitionRoots.empty();
    if (!partitioned)
      return {};
  }
  SHA256 hash;
  hash.update("llvm.coff.binding-context.1");
  SmallVector<BindingEntity *, 0> ordered;
  for (const auto &entry : bindingEntities)
    ordered.push_back(entry.second);
  sort(ordered, compareEntities);
  hashBindingNumber(hash, ordered.size());
  for (BindingEntity *entity : ordered) {
    hashBindingField(hash, entity->identity);
    uint8_t properties[] = {uint8_t(entity->owner), uint8_t(entity->flags)};
    hash.update(properties);
    hashBindingField(hash, entity->contract ? entity->contract->bytes : "");
    hashBindingField(hash, entity->import ? entity->import->getDLLName() : "");
    hashBindingField(hash,
                     entity->import ? entity->import->getExternalName() : "");
  }
  // Native inputs contribute constraints unavailable to ThinLTO's summary.
  // An unchanged bitcode module must not reuse code generated for a different
  // native closure or final-output boundary. Each field is length-delimited.
  hashBindingField(hash, "native-inputs");
  size_t count = 0;
  for (ObjFile *file : ctx.objFileInstances)
    count += &file->symtab == this;
  hashBindingNumber(hash, count);
  for (ObjFile *file : ctx.objFileInstances)
    if (&file->symtab == this)
      hashBindingField(hash, file->mb.getBuffer());
  hashBindingField(hash, "partition-roots");
  count = 0;
  for (BitcodeFile *file : bitcodeFileInstances)
    count += file->partitionRoots.size();
  hashBindingNumber(hash, count);
  for (BitcodeFile *file : bitcodeFileInstances)
    for (const PartitionRoot &root : file->partitionRoots) {
      hashBindingField(hash, root.symbol->getName());
      hashBindingField(hash, root.name);
    }
  return toHex(hash.final(), true);
}

static StringRef getProviderName(MachineTypes machine, StringRef identity) {
  SHA256 hash;
  // This is the ABI namespace, not a package, layout digest or output path.
  // The full, unhashed semantic spelling is also the required named export.
  hash.update("llvm.itanium.coff.rtti2");
  uint8_t arch[2];
  write16le(arch, machine);
  hash.update(arch);
  hash.update(identity);
  // Native module lookup is case-insensitive: do not encode digest bits in
  // filename case. The digest selects a filename, never authenticates a DLL.
  return saver().save("rtti2-" + toHex(hash.final(), /*LowerCase=*/true) +
                      ".dll");
}

static DefinedImportData *getPublishedImport(SymbolTable &symtab,
                                             BindingEntity &entity) {
  if (entity.import)
    return entity.import;
  // A metadata output may refer back to a published owner without making that
  // owner's own accesses imported. This is an output-local import view, not a
  // competing __imp_ definition in the shared symbol table.
  if (!entity.hasNamedExport) {
    Export exp;
    exp.name = entity.symbol->getName();
    exp.exportAs = entity.lookupName;
    exp.sym = entity.symbol;
    exp.data = true;
    symtab.exports.push_back(exp);
    entity.hasNamedExport = true;
  }
  StringRef dll = saver().save(symtab.ctx.driver.getImportName(false));
  entity.import = ImportFile::create(symtab.ctx, entity.symbol->getName(), dll,
                                     entity.lookupName, IMPORT_DATA,
                                     /*registerSymbols=*/false)
                      ->impSym;
  return entity.import;
}

static void checkProviderCycles(SymbolTable &symtab) {
  DenseMap<ImportFile *, BindingProvider *> providers;
  for (BindingProvider *provider : symtab.bindingProviders)
    if (provider->live)
      providers[provider->entity.import->file] = provider;
  DenseMap<BindingProvider *, uint8_t> state;
  SmallVector<std::pair<BindingProvider *, size_t>, 16> stack;
  for (BindingProvider *root : symtab.bindingProviders) {
    if (!root->live || state.lookup(root))
      continue;
    state[root] = 1;
    stack.emplace_back(root, 0);
    while (!stack.empty()) {
      auto &[provider, index] = stack.back();
      if (index == provider->references.size()) {
        state[provider] = 2;
        stack.pop_back();
        continue;
      }
      BindingProvider *dependency =
          providers.lookup(provider->references[index++].target->file);
      if (!dependency)
        continue;
      if (state.lookup(dependency) == 1)
        Fatal(symtab.ctx) << "cyclic canonical provider dependency from "
                          << provider->entity.symbol->getName() << " to "
                          << dependency->entity.symbol->getName();
      if (!state.lookup(dependency)) {
        state[dependency] = 1;
        stack.emplace_back(dependency, 0);
      }
    }
  }
}

void SymbolTable::prepareBindingOwners() {
  validateABIContracts();
  // LTO replaces a prevailing node in place, but native weak emission can
  // select a newly created fallback node. Reattach the already chosen owner
  // through the originating requirement before collecting native evidence.
  // The provider identity must never become an assembler fallback spelling.
  DenseMap<Symbol *, BindingEntity *> selected;
  for (const auto &entry : bindingEntities) {
    BindingEntity *entity = entry.second;
    Symbol *target = getBindingTarget(entity->requirement);
    for (Symbol *requirement : entity->requirements) {
      Symbol *selectedTarget = getBindingTarget(requirement);
      if (!sameDefinition(selectedTarget, target))
        Fatal(ctx) << "canonical requirement " << requirement->getName()
                   << " changed its selected target after ownership of "
                   << entity->identity << " was frozen";
    }
    auto [it, inserted] = selected.try_emplace(target, entity);
    if (!inserted && it->second != entity)
      Fatal(ctx) << "canonical requirements for " << entity->identity << " and "
                 << it->second->identity
                 << " selected one definition after ownership was frozen";
    entity->symbol = target;
    entity->contract = abiContracts.lookup(target);
  }
  bindingEntities = std::move(selected);
  for (ObjFile *file : ctx.objFileInstances)
    if (&file->symtab == this)
      collectBindingEntities(file, file->getBindingRequirements());
  for (BitcodeFile *file : bitcodeFileInstances)
    collectBindingEntities(file, file->getBindingRequirements());

  SmallPtrSet<Symbol *, 32> published;
  for (const Export &exp : exports) {
    Symbol *symbol = exp.sym ? exp.sym : find(exp.name);
    Defined *target =
        symbol ? dyn_cast<Defined>(getBindingTarget(symbol)) : nullptr;
    if (!target || !exp.forwardTo.empty())
      continue;
    published.insert(target);
    auto it = bindingEntities.find(target);
    if (it != bindingEntities.end() && exp.name == target->getName() &&
        exp.extName.empty() && exp.exportAs.empty() && !exp.noname)
      it->second->hasNamedExport = true;
  }

  SmallVector<BindingEntity *, 0> ordered;
  for (auto &entry : bindingEntities)
    ordered.push_back(entry.second);
  llvm::sort(ordered, compareEntities);
  for (BindingEntity *entity : ordered) {
    entity->lookupName = entity->contract
                             ? saver().save(entity->identity + "$abi$" +
                                            Twine(abi::ContractVersion) + "$" +
                                            entity->contract->digest)
                             : entity->identity;
    if (entity->import && entity->contract)
      entity->import->file->requireABIContract(entity->lookupName);
    if (!(entity->flags & BindingCanonical) ||
        entity->owner != BindingEntity::Unresolved)
      continue;
    Symbol *symbol = entity->symbol;
    auto *regular = dyn_cast<DefinedRegular>(symbol);
    // A strong definition wins under ordinary COFF rules, including when a
    // weak wrapper has an offered import. Only a replaceable definition may
    // select that alternative.
    auto *import = regular && !isReplaceableBinding(regular)
                       ? nullptr
                       : findBindingImport(*this, *entity);
    if (import) {
      entity->owner = BindingEntity::Imported;
      entity->import = import;
      if (entity->contract)
        import->file->requireABIContract(entity->lookupName);
    } else if (regular && published.contains(symbol)) {
      // Preserve key-function and explicitly published owners. A current
      // single consumer or hidden visibility does not establish a private set.
      entity->owner = BindingEntity::Published;
    } else if (regular && ctx.config.privateRTTI.contains(entity->identity)) {
      entity->owner = BindingEntity::Private;
    } else if (regular && !isReplaceableBinding(regular)) {
      entity->owner = BindingEntity::Published;
    } else if (regular) {
      if (isEC() || ctx.hybridSymtab || !ctx.config.importSlots)
        Fatal(ctx)
            << "generated canonical provider for " << symbol->getName()
            << " requires native import slots on a supported architecture";
      entity->owner = BindingEntity::Generated;
      StringRef dll = getProviderName(machine, entity->identity);
      entity->provider = make<BindingProvider>(*entity, dll);
      bindingProviders.push_back(entity->provider);
      entity->import = ImportFile::create(ctx, symbol->getName(), dll,
                                          entity->lookupName, IMPORT_DATA)
                           ->impSym;
    }
  }
  for (Export &exp : exports) {
    Symbol *symbol = exp.sym ? exp.sym : find(exp.name);
    if (!symbol || !exp.forwardTo.empty())
      continue;
    BindingEntity *entity = bindingEntities.lookup(getBindingTarget(symbol));
    if (entity && entity->contract &&
        entity->owner == BindingEntity::Published &&
        exp.name == entity->identity && !exp.noname)
      exp.exportAs = entity->lookupName;
  }
  // Private output-set relationships were checked above and cannot be
  // independently replaced. A layout-only use adds no native lookup there.
  for (ObjFile *file : ctx.objFileInstances) {
    if (!owns(file))
      continue;
    for (auto &entry : file->abiUses) {
      auto &uses = entry.second;
      size_t kept = 0;
      for (uint32_t index : uses) {
        BindingEntity *entity =
            bindingEntities.lookup(getBindingTarget(file->getSymbol(index)));
        if (!entity || entity->owner != BindingEntity::Private)
          uses[kept++] = index;
      }
      uses.resize(kept);
    }
  }
}

void SymbolTable::normalizeBindingReferences() {
  // Resolve coordinates while the original definitions still exist. A
  // section symbol or an interior label plus an addend can name precisely the
  // same byte as an exported object. This is relocation equivalence, not an
  // alias between the symbols themselves (nor between a cell and its pointee).
  DenseMap<ChunkAndOffset, Symbol *> targets;
  for (const auto &entry : symMap) {
    auto *def = dyn_cast<DefinedRegular>(getBindingTarget(entry.second));
    if (!def || !def->data || !isa<ObjFile>(def->getFile()))
      continue;
    if (!hasCanonicalBinding(def) && !isa_and_nonnull<DefinedImportData>(find(
                                         ("__imp_" + def->getName()).str())))
      continue;
    auto [it, inserted] =
        targets.try_emplace({def->getChunk(), def->getValue()}, def);
    // Choose a stable spelling without discarding either alias's requirements.
    // Owner compatibility remains the responsibility of binding resolution.
    if (!inserted && def->getName() < it->second->getName())
      it->second = def;
  }
  if (targets.empty())
    return;

  for (ObjFile *file : ctx.objFileInstances) {
    if (!owns(file))
      continue;
    DenseMap<Symbol *, uint32_t> indices;
    for (Chunk *chunk : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(chunk);
      if (!sc || !sc->live || sc->isCodeView() || sc->isDWARF())
        continue;
      sc->sortRelocations();
      ArrayRef<coff_relocation> relocs = sc->getRelocs();
      MutableArrayRef<coff_relocation> replacements;
      uint64_t previousEnd = 0;
      for (auto [i, rel] : enumerate(relocs)) {
        unsigned extent = sc->getRelocationWidth(rel);
        bool overlap = extent && rel.VirtualAddress < previousEnd;
        if (extent)
          previousEnd =
              std::max(previousEnd, uint64_t(rel.VirtualAddress) + extent);
        Symbol *symbol = file->getSymbol(rel.SymbolTableIndex);
        auto *def = symbol
                        ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                        : nullptr;
        if (!def || !def->data)
          continue;
        std::optional<int64_t> addend = sc->getPointerAddend(rel);
        unsigned width = ctx.config.wordsize;
        ArrayRef<uint8_t> contents = sc->getContents();
        if (!addend && sc->getArch() == Triple::x86_64 &&
            rel.Type >= IMAGE_REL_AMD64_REL32 &&
            rel.Type <= IMAGE_REL_AMD64_REL32_5 &&
            rel.VirtualAddress <= contents.size() &&
            contents.size() - rel.VirtualAddress >= 4) {
          addend = int32_t(read32le(contents.data() + rel.VirtualAddress));
          width = 4;
        }
        int64_t offset;
        if (!addend || AddOverflow(int64_t(def->getValue()), *addend, offset) ||
            offset < 0 || uint64_t(offset) >= def->getChunk()->getSize())
          continue;
        if (uint64_t(offset) > UINT32_MAX)
          continue;
        Symbol *target = targets.lookup({def->getChunk(), uint32_t(offset)});
        if (!target || (target == symbol && !*addend))
          continue;
        for (size_t next = i + 1;
             next < relocs.size() && uint64_t(relocs[next].VirtualAddress) <
                                         uint64_t(rel.VirtualAddress) + width;
             ++next)
          overlap |= sc->getRelocationWidth(relocs[next]) != 0;
        if (overlap)
          Fatal(ctx) << toString(file)
                     << ": overlapping canonical reference at "
                     << sc->getSectionName() << "+" << rel.VirtualAddress;
        if (replacements.empty()) {
          ArrayRef<coff_relocation> original = sc->getRelocs();
          replacements = {bAlloc().Allocate<coff_relocation>(original.size()),
                          original.size()};
          llvm::copy(original, replacements.begin());
        }
        auto [it, inserted] = indices.try_emplace(target, 0);
        if (inserted)
          it->second = file->addSyntheticSymbol(target);
        replacements[i].SymbolTableIndex = it->second;
        if (*addend) {
          MutableArrayRef<uint8_t> &bytes = file->rewrittenContents[sc->header];
          if (bytes.empty()) {
            bytes = {bAlloc().Allocate<uint8_t>(contents.size()),
                     contents.size()};
            llvm::copy(contents, bytes.begin());
          }
          memset(bytes.data() + rel.VirtualAddress, 0, width);
        }
      }
      if (!replacements.empty())
        sc->setRelocs(replacements);
    }
  }
}

void SymbolTable::materializeBindingProviders() {
  // GC has walked the original contributions, including all descriptor edges.
  // Capture the selected bodies before native binding replaces symbol bodies.
  for (BindingProvider *provider : bindingProviders) {
    auto *def = dyn_cast<DefinedRegular>(provider->entity.symbol);
    if (!def || !def->getChunk()->live)
      continue;
    SectionChunk *chunk = def->getChunk();
    if (def->getValue() || chunk->getContents().empty() ||
        (chunk->getOutputCharacteristics() & permMask) != IMAGE_SCN_MEM_READ ||
        !chunk->children().empty())
      Fatal(ctx) << "canonical provider contribution for " << def->getName()
                 << " must be a separate immutable object";
    provider->live = true;
    provider->contribution = chunk;
    provider->contents = chunk->getContents();
    provider->setAlignment(chunk->getAlignment());
    uint64_t previousEnd = 0;
    for (const auto &rel : chunk->getRelocs()) {
      uint32_t offset = rel.VirtualAddress;
      std::optional<int64_t> addend = chunk->getPointerAddend(rel);
      if (!addend || offset < previousEnd || offset % ctx.config.wordsize)
        Fatal(ctx) << "unsupported canonical provider relocation in "
                   << def->getName();
      previousEnd = uint64_t(offset) + ctx.config.wordsize;
      if (*addend)
        Fatal(ctx) << "canonical provider " << def->getName()
                   << " requires an exact, zero-addend dependency";
      ArrayRef<Symbol *> symbols = chunk->file->getSymbols();
      if (rel.SymbolTableIndex >= symbols.size() ||
          !symbols[rel.SymbolTableIndex])
        Fatal(ctx) << "invalid canonical provider relocation symbol in "
                   << def->getName();
      Symbol *target = getBindingTarget(symbols[rel.SymbolTableIndex]);
      auto *import = dyn_cast_or_null<DefinedImportData>(target);
      if (!import) {
        auto it = bindingEntities.find(target);
        if (it != bindingEntities.end()) {
          BindingEntity &dependency = *it->second;
          import = dependency.owner == BindingEntity::Published
                       ? getPublishedImport(*this, dependency)
                       : dependency.import;
        }
        if (!import && target)
          import = dyn_cast_or_null<DefinedImportData>(
              find(("__imp_" + target->getName()).str()));
      }
      if (!import || (target && target->getName().starts_with("__imp_")))
        Fatal(ctx) << "canonical provider " << def->getName()
                   << " has no published native dependency for "
                   << chunk->file->getSymbol(rel.SymbolTableIndex)->getName();
      if (ctx.config.delayLoads.contains(import->getDLLName().lower()))
        Fatal(ctx) << "canonical provider dependency " << import->getDLLName()
                   << " cannot be delay loaded";
      provider->references.push_back({offset, import});
    }
  }
  checkProviderCycles(*this);
}

bool SymbolTable::retireBindingContributions() {
  SmallPtrSet<SectionChunk *, 32> candidates;
  for (const auto &entry : canonicalResiduals)
    candidates.insert(entry.first);
  // Symbol-table entries and debug descriptions do not retain storage. An
  // unreferenced interior label must not defeat GC of an otherwise transferred
  // object. Explicit roots still retain their indivisible contribution.
  for (Symbol *root : ctx.config.gcroot)
    if (auto *def = dyn_cast_or_null<DefinedRegular>(root->getDefined()))
      if (def->data)
        candidates.erase(def->getChunk());
  SmallVector<SectionChunk *, 16> worklist;
  for (ObjFile *file : ctx.objFileInstances) {
    if (!owns(file))
      continue;
    for (Chunk *chunk : file->getChunks())
      if (auto *sc = dyn_cast<SectionChunk>(chunk))
        if (sc->live && !sc->isCodeView() && !sc->isDWARF() &&
            !candidates.contains(sc))
          worklist.push_back(sc);
  }
  while (!worklist.empty()) {
    SectionChunk *sc = worklist.pop_back_val();
    for (Symbol *symbol : sc->dependencies()) {
      auto *def = symbol
                      ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                      : nullptr;
      if (def && def->data && candidates.erase(def->getChunk()))
        worklist.push_back(def->getChunk());
    }
    for (SectionChunk &child : sc->children())
      if (candidates.erase(&child))
        worklist.push_back(&child);
  }
  for (SectionChunk *chunk : candidates)
    chunk->live = false;
  return !candidates.empty();
}

void BindingProvider::writeTo(uint8_t *buf) const {
  memcpy(buf, contents.data(), contents.size());
  for (const Reference &reference : references)
    reference.target->file->lookup->writeTo(buf + reference.offset);
}

static bool isNativeAddressReference(SectionChunk &chunk,
                                     const object::coff_relocation &rel) {
  if (chunk.getImportRefForm(rel) == SectionChunk::ImportRefForm::Lea)
    return true;
  if (chunk.getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE)
    return false;
  return chunk.getPointerAddend(rel) == std::optional<int64_t>(0);
}

void SymbolTable::bindSharedWeakDataGroups() {
  struct Group {
    SectionChunk *leader;
    DefinedImportData *owner;
  };
  EquivalenceClasses<SectionChunk *> associations;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this || !file->hasRTTIABI())
      continue;
    for (Chunk *chunk : file->getChunks())
      if (auto *sc = dyn_cast<SectionChunk>(chunk))
        for (SectionChunk &child : sc->children())
          associations.unionSets(sc, &child);
  }

  SmallVector<Group, 0> groups;
  DenseMap<SectionChunk *, unsigned> indices;
  DenseMap<std::pair<SectionChunk *, uint32_t>, DefinedImportData *> targets;
  for (const auto &entry : symMap) {
    DefinedRegular *var = getCompilerStubTarget(*this, entry.second);
    // Final LTO lowering can already name __imp_ directly, eliminating the
    // compiler stub. The selected variable/guard group must still transfer
    // atomically, including its constructor registration.
    if (!var)
      var = dyn_cast_or_null<DefinedRegular>(entry.second->getDefined());
    if (!var || !var->data)
      continue;
    auto *file = dyn_cast_or_null<ObjFile>(var->getFile());
    if (!file || !file->hasRTTIABI() || !var->getChunk()->isCOMDAT())
      continue;
    if (var->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE)
      continue;
    if (BindingEntity *entity = bindingEntities.lookup(getBindingTarget(var)))
      if (entity->flags & BindingCanonical)
        continue;
    auto *import = dyn_cast_or_null<DefinedImportData>(
        find(("__imp_" + var->getName()).str()));
    if (!import)
      continue;
    SectionChunk *chunk = var->getChunk();
    associations.insert(chunk);
    SectionChunk *leader = associations.getLeaderValue(chunk);
    bool live = false;
    for (SectionChunk *member : associations.members(leader))
      live |= member->live;
    if (!live)
      continue;
    auto [it, inserted] = indices.try_emplace(leader, groups.size());
    if (inserted)
      groups.push_back({leader, import});
    else if (!groups[it->second].owner->getDLLName().equals_insensitive(
                 import->getDLLName()))
      Fatal(ctx) << "shared variable/guard group selects different providers: "
                 << var->getName() << " in " << import->getDLLName() << " and "
                 << groups[it->second].owner->getDLLName();
    auto [target, first] =
        targets.try_emplace(std::make_pair(chunk, var->getValue()), import);
    if (!first && !sameImportedTarget(target->second, import))
      Fatal(ctx) << "shared address aliases select different imports for "
                 << var->getName();
  }
  if (groups.empty())
    return;
  if (!ctx.config.importSlots || isEC() || ctx.hybridSymtab)
    Fatal(ctx) << "shared variable/guard binding requires native import slots";

  DenseMap<SectionChunk *, unsigned> members;
  for (auto [i, group] : enumerate(groups)) {
    if (ctx.config.delayLoads.contains(group.owner->getDLLName().lower()))
      Fatal(ctx) << "shared variable/guard provider cannot be delay loaded: "
                 << group.owner->getDLLName();
    for (SectionChunk *member : associations.members(group.leader))
      members[member] = i;
  }
  DenseMap<DefinedRegular *, DefinedImportData *> bindings;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this)
      continue;
    for (Symbol *symbol : file->getSymbols()) {
      auto *def = symbol
                      ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                      : nullptr;
      if (!def || !def->data || !members.contains(def->getChunk()))
        continue;
      auto *import = targets.lookup({def->getChunk(), def->getValue()});
      if (import)
        bindings[def] = import;
    }
    for (Chunk *chunk : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(chunk);
      if (!sc || !sc->live || sc->isCodeView() || sc->isDWARF())
        continue;
      for (const auto &rel : sc->getRelocs()) {
        Symbol *symbol = file->getSymbol(rel.SymbolTableIndex);
        auto *def = symbol
                        ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                        : nullptr;
        if (!def || !def->data)
          continue;
        auto target = members.find(def->getChunk());
        if (target == members.end())
          continue;
        auto source = members.find(sc);
        if (source != members.end() && source->second == target->second)
          continue;
        if (!bindings.contains(def) || !isNativeAddressReference(*sc, rel))
          Fatal(ctx) << toString(file)
                     << ": cannot redirect shared lifetime "
                        "group reference to "
                     << symbol->getName() << " at " << sc->getSectionName()
                     << "+" << rel.VirtualAddress << " to "
                     << groups[target->second].owner->getDLLName();
      }
    }
  }
  // Resolve all externally observable group members before dropping any
  // variable, guard or initializer. A compiler cell is outside the group: its
  // address survives, and bindCompilerStubs later optimizes individual reads.
  for (Export &exp : exports) {
    if (!exp.forwardTo.empty())
      continue;
    Symbol *symbol = exp.sym ? exp.sym : find(exp.name);
    auto *def = symbol ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                       : nullptr;
    if (!def || !def->data || !members.contains(def->getChunk()))
      continue;
    DefinedImportData *import = bindings.lookup(def);
    if (!import)
      Fatal(ctx) << "export " << exp.name
                 << " exposes an unbound member of a "
                    "shared variable/guard group";
    StringRef dll = import->getDLLName();
    dll.consume_back_insensitive(".dll");
    exp.forwardTo = saver().save(dll + "." + import->getExternalName());
  }
  for (Symbol *root : ctx.config.gcroot) {
    auto *def = dyn_cast_or_null<DefinedRegular>(root->getDefined());
    if (def && def->data && members.contains(def->getChunk()) &&
        !bindings.contains(def))
      Fatal(ctx) << "root " << root->getName()
                 << " prevents transfer of its shared variable/guard group";
  }
  DenseMap<Symbol *, Symbol *> redirects;
  for (const auto &[symbol, import] : bindings) {
    import->file->live = true;
    redirects[symbol] = makeImportAlias(symbol, import, /*pointee=*/true);
  }
  redirectSymbols(redirects);
  for (const auto &entry : members)
    entry.first->live = false;
}

} // namespace lld::coff
