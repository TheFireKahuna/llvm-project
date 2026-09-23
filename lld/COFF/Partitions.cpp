//===- Partitions.cpp - Final PE output views -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Partitions.h"
#include "Binding.h"
#include "COFFLinkerContext.h"
#include "Symbols.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"

using namespace llvm;
using namespace llvm::COFF;

namespace lld::coff {

namespace {
// A new PE needs its own load configuration even when its input members did
// not carry a CRT directory. Populate the standard fields from this output's
// existing linker symbols; there is no new loader protocol or runtime fixup.
class OutputLoadConfigChunk final : public NonSectionChunk {
public:
  explicit OutputLoadConfigChunk(SymbolTable &symbols) : symbols(symbols) {
    setAlignment(symbols.ctx.config.wordsize);
    if (symbols.ctx.config.is64())
      initialize<object::coff_load_configuration64>();
    else
      initialize<object::coff_load_configuration32>();
  }
  size_t getSize() const override { return size; }
  StringRef getSectionName() const override { return ".rdata"; }
  uint32_t getOutputCharacteristics() const override {
    return IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA;
  }
  void writeTo(uint8_t *buffer) const override {
    memset(buffer, 0, size);
    support::endian::write32le(buffer, size);
    for (const Field &field : fields) {
      auto *def = cast<Defined>(field.symbol);
      auto *absolute = dyn_cast<DefinedAbsolute>(def);
      uint64_t value = absolute ? absolute->getVA() : def->getRVA();
      if (field.address && !absolute)
        value += symbols.ctx.config.imageBase;
      if (field.width == 8)
        support::endian::write64le(buffer + field.offset, value);
      else
        support::endian::write32le(buffer + field.offset, value);
    }
  }
  void getBaserels(std::vector<Baserel> *relocs) override {
    for (const Field &field : fields)
      if (field.address && !isa<DefinedAbsolute>(field.symbol))
        relocs->emplace_back(getRVA() + field.offset, symbols.machine);
  }

private:
  struct Field {
    Symbol *symbol;
    unsigned offset;
    unsigned width;
    bool address;
  };
  void add(StringRef name, unsigned offset, unsigned width, bool address) {
    if (Symbol *symbol = symbols.findUnderscore(name))
      if (isa<Defined>(symbol))
        fields.push_back({symbol, offset, width, address});
  }
  template <class T> void initialize() {
    const Configuration &config = symbols.ctx.config;
    // Include only the last required standard field. Pointer-width padding
    // and later, unused platform extensions are not part of this directory.
    size = offsetof(T, SEHandlerCount) + sizeof(T::SEHandlerCount);
#define ADD(F, N, A) add(N, offsetof(T, F), sizeof(T::F), A)
    ADD(SecurityCookie, "__security_cookie", true);
    ADD(SEHandlerTable, "__safe_se_handler_table", true);
    ADD(SEHandlerCount, "__safe_se_handler_count", false);
    if (config.guardCF != GuardCFLevel::Off) {
      size = offsetof(T, GuardAddressTakenIatEntryCount) +
             sizeof(T::GuardAddressTakenIatEntryCount);
      ADD(GuardCFCheckFunction, "__guard_check_icall_fptr", true);
      ADD(GuardCFCheckDispatch, "__guard_dispatch_icall_fptr", true);
      ADD(GuardCFFunctionTable, "__guard_fids_table", true);
      ADD(GuardCFFunctionCount, "__guard_fids_count", false);
      ADD(GuardFlags, "__guard_flags", false);
      ADD(GuardAddressTakenIatEntryTable, "__guard_iat_table", true);
      ADD(GuardAddressTakenIatEntryCount, "__guard_iat_count", false);
    }
    if (config.guardCF & GuardCFLevel::LongJmp) {
      size = offsetof(T, GuardLongJumpTargetCount) +
             sizeof(T::GuardLongJumpTargetCount);
      ADD(GuardLongJumpTargetTable, "__guard_longjmp_table", true);
      ADD(GuardLongJumpTargetCount, "__guard_longjmp_count", false);
    }
    if (config.guardCF & GuardCFLevel::EHCont) {
      size = offsetof(T, GuardEHContinuationCount) +
             sizeof(T::GuardEHContinuationCount);
      ADD(GuardEHContinuationTable, "__guard_eh_cont_table", true);
      ADD(GuardEHContinuationCount, "__guard_eh_cont_count", false);
    }
#undef ADD
  }
  SymbolTable &symbols;
  unsigned size;
  SmallVector<Field, 12> fields;
};
} // namespace

OutputPartition::OutputPartition(COFFLinkerContext &ctx, StringRef name,
                                 unsigned index)
    : name(name), index(index), symbols(ctx, ctx.config.machine) {
  symbols.resolutionSource = &ctx.symtab;
  symbols.hadExplicitExports = true;
}

void Partitioning::collectRoots(InputFile &file) {
  for (const PartitionRoot &root : file.partitionRoots) {
    if (root.name.starts_with("dll:") || root.name == "image:")
      Fatal(ctx) << "partition name uses the reserved provider prefix: "
                 << root.name;
    // Compiler partition labels on old native/bitcode inputs may also name
    // RTTI. Ownership already classifies those symbols; they are movable
    // metadata dependencies, not additional public code roots or GC roots.
    Symbol *target = getBindingTarget(root.symbol);
    if (ctx.symtab.bindingEntities.contains(target) ||
        ctx.symtab.bindingRequirements.contains(root.symbol))
      continue;
    Symbol *symbol = root.symbol->getDefined();
    if (!symbol)
      Fatal(ctx) << toString(&file) << ": partition root "
                 << root.symbol->getName() << " has no definition";
    auto [it, inserted] = roots.try_emplace(symbol, root.name);
    if (!inserted && it->second != root.name)
      Fatal(ctx) << "conflicting partitions for " << symbol->getName() << ": "
                 << it->second << " and " << root.name;
    if (inserted) {
      symbol->isUsedInRegularObj = true;
      symbol->isGCRoot = true;
      ctx.config.gcroot.push_back(symbol);
    }
  }
}

void Partitioning::prepare() {
  for (ObjFile *file : ctx.objFileInstances) {
    if (file->imageLocal)
      for (Chunk *chunk : file->getChunks())
        imageTemplates.insert(chunk);
    for (const PartitionRoot &placement : file->placements)
      if (placement.name == "image:")
        if (Defined *def = placement.symbol->getDefined())
          if (def->getChunk())
            imageTemplates.insert(def->getChunk());
  }
  SmallVector<const Chunk *, 32> pending(imageTemplates.begin(),
                                         imageTemplates.end());
  while (!pending.empty())
    if (auto *section = dyn_cast<SectionChunk>(pending.pop_back_val()))
      for (SectionChunk &child : section->children())
        if (imageTemplates.insert(&child).second)
          pending.push_back(&child);
  for (const auto &entry : ctx.symtab.bindingEntities)
    if (auto *def = dyn_cast<DefinedRegular>(entry.first))
      if (isImageLocal(def->getChunk()))
        Fatal(ctx) << "per-image runtime contribution contains RTTI identity "
                   << def->getName();
  for (ObjFile *file : ctx.objFileInstances)
    collectRoots(*file);
  for (BitcodeFile *file : ctx.symtab.bitcodeFileInstances)
    collectRoots(*file);
  if (roots.empty())
    return;
  if (ctx.hybridSymtab || ctx.symtab.isEC())
    Fatal(ctx) << "final PE partitions require a non-hybrid target";
  SmallVector<StringRef, 0> names;
  for (const auto &root : roots)
    names.push_back(root.second);
  sort(names);
  names.erase(std::unique(names.begin(), names.end()), names.end());
  if (outputs.empty())
    outputs.push_back(make<OutputPartition>(ctx, "", 1));
  for (StringRef name : names) {
    bool found = false;
    for (OutputPartition *output : outputs)
      found |= output->name == name;
    if (!found)
      outputs.push_back(make<OutputPartition>(ctx, name, outputs.size() + 1));
  }
}

unsigned Partitioning::owner(const Chunk *chunk) const {
  return owners.lookup(leader(chunk));
}

const Chunk *Partitioning::leader(const Chunk *chunk) const {
  auto *mutableChunk = const_cast<Chunk *>(chunk);
  return groups.contains(mutableChunk) ? groups.getLeaderValue(mutableChunk)
                                       : chunk;
}

static std::optional<uint32_t>
getExactDataOffset(SectionChunk &chunk, const object::coff_relocation &rel,
                   Defined &target) {
  // A section bound alone can include unrelated objects or alignment bytes.
  // Use the selected producer's emitted object extent for a semantic interior.
  auto *def = dyn_cast<DefinedRegular>(&target);
  if (!def ||
      (def->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
    return std::nullopt;
  SectionChunk *storage = def->getChunk();
  // ICF can redirect a definition to another input's selected contribution.
  // Bounds follow actual storage, not the original symbol's input-file pointer.
  ObjFile *file = storage->file;
  if (!file || !file->symtab.ctx.config.importSlots)
    return std::nullopt;
  std::optional<int64_t> addend = chunk.getPointerAddend(rel);
  if (chunk.getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE) {
    // Materializing an exact data address has the existing equal-size LEA to
    // MOV alternative. A load of the data itself does not: it needs a separate
    // pointer acquisition and must retain its ordinary placement constraint.
    if (chunk.getImportRefForm(rel, /*ignoreAddend=*/true) !=
        SectionChunk::ImportRefForm::Lea)
      return std::nullopt;
    addend = int32_t(support::endian::read32le(
        chunk.getContents().data() + rel.VirtualAddress));
  }
  bool sectionRelative = def->getCOFFSymbol().isSectionDefinition();
  if (!addend || (!*addend && !sectionRelative))
    return std::nullopt;
  int64_t offset;
  if (AddOverflow(int64_t(def->getValue()), *addend, offset) || offset < 0 ||
      uint64_t(offset) > UINT32_MAX)
    return std::nullopt;
  auto extent = file->getObjectExtent(storage, uint32_t(offset));
  if (!extent)
    return std::nullopt;
  // An interior label may move in either direction within its actual object.
  // A section symbol has no object identity: its resolved coordinate supplies
  // that identity. A named object's out-of-bounds offset must not acquire the
  // next object's extent merely because it is adjacent in the section.
  if (!sectionRelative && file->getObjectExtent(storage, def->getValue()) != extent)
    return std::nullopt;
  return uint32_t(offset);
}

bool canImportPartitionReference(SectionChunk &chunk,
                                 const object::coff_relocation &rel,
                                 Defined &target) {
  // Debug coordinates are image-local descriptions, never binding edges.
  if (chunk.isCodeView() || chunk.isDWARF())
    return true;
  ArrayRef<uint8_t> data = chunk.getContents();
  uint32_t offset = rel.VirtualAddress;
  auto *regular = dyn_cast<DefinedRegular>(&target);
  bool sectionRelative = regular && isa_and_nonnull<ObjFile>(regular->getFile()) &&
                         regular->getCOFFSymbol().isSectionDefinition();
  if (!(chunk.getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE)) {
    if (sectionRelative)
      return getExactDataOffset(chunk, rel, target).has_value();
    return chunk.getPointerAddend(rel) == std::optional<int64_t>(0) ||
           getExactDataOffset(chunk, rel, target).has_value();
  }
  if ((!sectionRelative &&
       chunk.getImportRefForm(rel) == SectionChunk::ImportRefForm::Lea) ||
      getExactDataOffset(chunk, rel, target).has_value())
    return true;
  if (!(target.getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
    return false;
  if (chunk.getMachine() == AMD64 && rel.Type == IMAGE_REL_AMD64_REL32 &&
      offset && offset <= data.size() && 4 <= data.size() - offset &&
      !support::endian::read32le(data.data() + offset))
    return data[offset - 1] == 0xe8 || data[offset - 1] == 0xe9;
  return (chunk.getMachine() == ARM64 &&
          rel.Type == IMAGE_REL_ARM64_BRANCH26) ||
         (chunk.getMachine() == ARMNT && rel.Type == IMAGE_REL_ARM_BRANCH24T) ||
         (chunk.getMachine() == I386 && rel.Type == IMAGE_REL_I386_REL32);
}

static Defined *placementTarget(Symbol *symbol) {
  Defined *target = symbol ? symbol->getDefined() : nullptr;
  if (auto *local = dyn_cast_or_null<DefinedLocalImport>(target))
    if (!local->getChunk()->live)
      target = local->wrappedSym;
  return target;
}

void Partitioning::placeSharedPrivateEntities() {
  // Promotion to main is cheapest only if it does not create a dependency
  // cycle. Find the eager closure first; optional export forwarders do not
  // belong to it. A shared immutable private contribution can instead occupy
  // one common output, preserving the optional partitions' initialization.
  SmallVector<SmallVector<unsigned, 4>, 0> edges(outputs.size() + 1);
  DenseMap<const Chunk *, SmallVector<unsigned, 2>> consumers;
  for (Chunk *chunk : ctx.driver.getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(chunk);
    unsigned source = owner(chunk);
    if (!sc || !sc->live || !source || sc->isDWARF() || sc->isCodeView())
      continue;
    for (Symbol *symbol : sc->dependencies()) {
      Defined *target = placementTarget(symbol);
      if (!isa_and_nonnull<DefinedRegular, DefinedCommon, DefinedLocalImport>(
              target))
        continue;
      unsigned destination = owner(target->getChunk());
      if (!destination || destination == source)
        continue;
      edges[source].push_back(destination);
      consumers[leader(target->getChunk())].push_back(source);
    }
  }
  SmallVector<bool, 0> reachable(outputs.size() + 1, false);
  SmallVector<unsigned, 16> pending{1};
  while (!pending.empty()) {
    unsigned current = pending.pop_back_val();
    if (reachable[current])
      continue;
    reachable[current] = true;
    append_range(pending, edges[current]);
  }
  DenseSet<const Chunk *> movable;
  for (const auto &entry : ctx.symtab.bindingEntities) {
    if (entry.second->owner != BindingEntity::Private)
      continue;
    auto *def = dyn_cast<DefinedRegular>(entry.first);
    if (!def || !def->data || !def->getChunk()->live)
      continue;
    const Chunk *group = leader(def->getChunk());
    if (fixed.contains(group) || owner(group) != 1)
      continue;
    bool immutable = true;
    if (groups.contains(def->getChunk())) {
      for (Chunk *member : groups.members(def->getChunk()))
        immutable &= (member->getOutputCharacteristics() & permMask) ==
                     IMAGE_SCN_MEM_READ;
    } else {
      immutable =
          (group->getOutputCharacteristics() & permMask) == IMAGE_SCN_MEM_READ;
    }
    if (immutable)
      movable.insert(group);
  }
  SmallVector<const Chunk *, 0> move;
  for (const auto &entry : consumers) {
    if (!movable.contains(entry.first))
      continue;
    for (unsigned consumer : entry.second)
      if (consumer != 1 && reachable[consumer]) {
        move.push_back(entry.first);
        break;
      }
  }
  if (move.empty())
    return;
  auto *shared = make<OutputPartition>(ctx, "", outputs.size() + 1);
  outputs.push_back(shared);
  while (!move.empty()) {
    const Chunk *group = move.pop_back_val();
    if (owner(group) == shared->index)
      continue;
    owners[group] = shared->index;
    SmallVector<const Chunk *, 4> members;
    if (groups.contains(const_cast<Chunk *>(group)))
      for (Chunk *member : groups.members(const_cast<Chunk *>(group)))
        members.push_back(member);
    else
      members.push_back(group);
    for (const Chunk *member : members) {
      if (auto *sc = dyn_cast<SectionChunk>(member)) {
        for (Symbol *symbol : sc->dependencies()) {
          Defined *target = placementTarget(symbol);
          if (target && isa<DefinedRegular>(target)) {
            const Chunk *dependency = leader(target->getChunk());
            if (movable.contains(dependency))
              move.push_back(dependency);
          }
        }
      }
    }
  }
}

void Partitioning::collectPlacementConstraints() {
  DenseMap<DefinedImportData *, SectionChunk *> observableCells;
  for (DefinedLocalImport *local : ctx.symtab.localImports)
    if (local->getChunk()->live && !isImageLocal(local->wrappedSym->getChunk()))
      groups.unionSets(local->getChunk(), local->wrappedSym->getChunk());
  for (Chunk *chunk : ctx.driver.getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(chunk);
    if (!sc || !sc->live || isImageLocal(sc) || sc->isCodeView() ||
        sc->isDWARF())
      continue;
    for (SectionChunk &child : sc->children())
      groups.unionSets(sc, &child);
    for (const auto &rel : sc->getRelocs()) {
      Symbol *symbol = sc->file->getSymbol(rel.SymbolTableIndex);
      Defined *target = symbol ? symbol->getDefined() : nullptr;
      if (auto *import = dyn_cast_or_null<DefinedImportData>(target)) {
        if (!import->isRuntimePseudoReloc) {
          auto form = sc->getImportRefForm(rel);
          if (form == SectionChunk::ImportRefForm::None ||
              form == SectionChunk::ImportRefForm::Lea) {
            auto [it, inserted] =
                observableCells.try_emplace(import->file->impSym, sc);
            if (!inserted)
              groups.unionSets(sc, it->second);
          }
        }
        continue;
      }
      if (auto *local = dyn_cast_or_null<DefinedLocalImport>(target)) {
        // Unobservable compiler cells are implementation details. Observable
        // cells are actual objects; never duplicate one cell per output.
        if (!local->getChunk()->live)
          continue;
      } else if (!isa_and_nonnull<DefinedRegular, DefinedCommon>(target)) {
        continue;
      }
      if (!isImageLocal(target->getChunk()) &&
          !canImportPartitionReference(*sc, rel, *target))
        groups.unionSets(sc, target->getChunk());
    }
  }
  for (const Export &exp : ctx.symtab.exports) {
    Symbol *symbol = exp.sym ? exp.sym : ctx.symtab.find(exp.name);
    auto *import = dyn_cast_or_null<DefinedImportData>(symbol);
    if (!import || import->isRuntimePseudoReloc || !exp.forwardTo.empty())
      continue;
    if (SectionChunk *user = observableCells.lookup(import->file->impSym))
      fixed[leader(user)] = 1;
  }
}

void Partitioning::enqueue(Chunk *chunk, unsigned partition) {
  if (!chunk || isImageLocal(chunk))
    return;
  if (auto *sc = dyn_cast<SectionChunk>(chunk)) {
    if (!sc->live || sc->isDWARF() || sc->isCodeView())
      return;
    chunk = sc->repl;
  }
  chunk = const_cast<Chunk *>(leader(chunk));
  unsigned pinned = fixed.lookup(chunk);
  if (pinned)
    partition = pinned;
  unsigned &current = owners[chunk];
  // ELF's reachability lattice: main < individual partition < unreached.
  // Unlike ELF, crossing this boundary later creates a native PE import.
  if (current == 1 || current == partition)
    return;
  current = current ? 1 : partition;
  if (groups.contains(chunk)) {
    for (Chunk *member : groups.members(chunk))
      worklist.push_back(member);
  } else {
    worklist.push_back(chunk);
  }
}

void Partitioning::visit(Symbol *symbol, unsigned partition) {
  Defined *def = symbol ? symbol->getDefined() : nullptr;
  if (!def)
    return;
  if (auto *local = dyn_cast<DefinedLocalImport>(def)) {
    if (local->getChunk()->live)
      enqueue(local->getChunk(), partition);
    visit(local->wrappedSym, partition);
    return;
  }
  if (!isa<DefinedRegular, DefinedCommon>(def))
    return;
  if (!isImageLocal(def->getChunk())) {
    enqueue(def->getChunk(), partition);
    return;
  }
  OutputPartition &output = *outputs[partition - 1];
  SmallVector<Chunk *, 16> pending{def->getChunk()};
  while (!pending.empty()) {
    Chunk *chunk = pending.pop_back_val();
    if (!output.imageChunks.insert(chunk).second)
      continue;
    if (auto *section = dyn_cast<SectionChunk>(chunk)) {
      for (SectionChunk &child : section->children())
        pending.push_back(&child);
      for (Symbol *dependency : section->dependencies()) {
        if (Symbol *projection = output.projections.lookup(dependency))
          dependency = projection;
        Defined *target = placementTarget(dependency);
        if (auto *local = dyn_cast_or_null<DefinedLocalImport>(target))
          target = local->wrappedSym;
        if (!target || !isa<DefinedRegular, DefinedCommon>(target))
          continue;
        if (isImageLocal(target->getChunk()))
          pending.push_back(target->getChunk());
        else
          enqueue(target->getChunk(), partition);
      }
    }
  }
}

void Partitioning::drain() {
  while (!worklist.empty()) {
    Chunk *chunk = worklist.pop_back_val();
    auto *sc = dyn_cast<SectionChunk>(chunk);
    if (!sc || !sc->live || sc->isCodeView() || sc->isDWARF())
      continue;
    unsigned partition = owner(sc);
    for (Symbol *symbol : sc->dependencies())
      visit(symbol, partition);
    for (SectionChunk &child : sc->children())
      enqueue(&child, partition);
  }
}

static void hashNumber(SHA256 &hash, uint64_t value) {
  uint8_t size[8];
  support::endian::write64le(size, value);
  hash.update(size);
}

static void hashField(SHA256 &hash, StringRef value) {
  hashNumber(hash, value.size());
  hash.update(value);
}

static void hashImport(SHA256 &hash, ImportFile &file) {
  hashField(hash, StringRef(file.dllName).lower());
  hashField(hash, file.externalName);
  uint8_t properties[4];
  support::endian::write16le(properties, file.hdr->TypeInfo);
  support::endian::write16le(properties + 2, file.hdr->OrdinalHint);
  hash.update(properties);
}

void Partitioning::nameOutputs() {
  SHA256 hash;
  hash.update("llvm.coff.output-set.2");
  // Length-delimit inputs: concatenation must not alias different input sets.
  // Include final native code as well as options, so backend and placement
  // changes cannot silently reuse incompatible private bindings.
  hashNumber(hash, ctx.objFileInstances.size());
  for (ObjFile *file : ctx.objFileInstances)
    hashField(hash, file->mb.getBuffer());
  hash.update("resolved-imports");
  size_t liveImports = 0;
  for (ImportFile *file : ctx.importFileInstances)
    liveImports += file->live;
  hashNumber(hash, liveImports);
  for (ImportFile *file : ctx.importFileInstances)
    if (file->live)
      hashImport(hash, *file);
  // Import libraries can change their DLL/export-as targets without changing
  // any native object or command-line spelling. Include provider dependencies
  // as well as main-image imports; both are part of the resolved output set.
  hash.update("canonical-providers");
  size_t liveProviders = 0;
  for (BindingProvider *provider : ctx.symtab.bindingProviders)
    liveProviders += provider->live;
  hashNumber(hash, liveProviders);
  for (BindingProvider *provider : ctx.symtab.bindingProviders) {
    if (!provider->live)
      continue;
    hashField(hash, provider->dllName);
    hashNumber(hash, provider->references.size());
    for (const BindingProvider::Reference &reference : provider->references) {
      uint8_t offset[4];
      support::endian::write32le(offset, reference.offset);
      hash.update(offset);
      hashImport(hash, *reference.target->file);
    }
  }
  hash.update("placement");
  hashNumber(hash, outputs.size());
  for (OutputPartition *output : outputs)
    hashField(hash, output->name);
  std::vector<Chunk *> chunks = ctx.driver.getChunks();
  hashNumber(hash, chunks.size());
  for (Chunk *chunk : chunks) {
    uint8_t index[4];
    support::endian::write32le(index, owner(chunk));
    hash.update(index);
    // Templates have no single owner. Their instantiated membership also
    // affects the output set, including startup and TLS directory contents.
    for (OutputPartition *output : outputs)
      hashNumber(hash, output->imageChunks.contains(chunk));
  }
  hash.update("options");
  hashNumber(hash, ctx.config.argv.size());
  for (StringRef arg : ctx.config.argv)
    hashField(hash, arg);
  std::string digest = toHex(hash.final(), true);
  outputs.front()->dllName = saver().save(ctx.driver.getImportName(false));
  for (OutputPartition *output : drop_begin(outputs))
    output->dllName =
        saver().save("part-" + digest + "-" + Twine(output->index) + ".dll");
}

void Partitioning::assign() {
  prepare();
  StringSet<> privateDefinitions;
  for (const auto &binding : ctx.symtab.bindingEntities)
    if (binding.second->owner == BindingEntity::Private)
      privateDefinitions.insert(binding.second->identity);
  for (const auto &entry : ctx.config.privateRTTI) {
    if (!privateDefinitions.contains(entry.getKey()))
      Fatal(ctx) << "/lldrttiprivate:" << entry.getKey()
                 << " does not name a private canonical definition";
  }
  if (empty())
    return;
  collectPlacementConstraints();
  for (const auto &[chunk, name] : frozenOwners) {
    if (isImageLocal(chunk))
      continue;
    if (auto *section = dyn_cast<SectionChunk>(chunk)) {
      if (!section->live)
        continue;
    } else if (auto *common = dyn_cast<CommonChunk>(chunk)) {
      if (!common->live)
        continue;
    }
    unsigned index = 0;
    for (OutputPartition *output : outputs)
      if (output->name == name)
        index = output->index;
    if (!index)
      Fatal(ctx) << "unknown frozen native PE output: " << name;
    auto [it, inserted] = fixed.try_emplace(leader(chunk), index);
    if (!inserted && it->second != index)
      Fatal(ctx) << "native constraints changed frozen PE placement";
    enqueue(chunk, index);
  }
  // These assignments were part of optimization and code generation, unlike
  // source partition roots. They constrain placement without retaining dead
  // definitions or creating public entry points.
  for (ObjFile *file : ctx.objFileInstances)
    for (const PartitionRoot &placement : file->placements) {
      Defined *def = placement.symbol->getDefined();
      if (!isa_and_nonnull<DefinedRegular, DefinedCommon>(def) ||
          !def->getChunk() || !def->isLive() || isImageLocal(def->getChunk()))
        continue;
      unsigned index = 0;
      for (OutputPartition *output : outputs)
        if (output->name == placement.name)
          index = output->index;
      if (!index)
        Fatal(ctx) << "unknown frozen PE output for " << def->getName();
      auto [it, inserted] = fixed.try_emplace(leader(def->getChunk()), index);
      if (!inserted && it->second != index)
        Fatal(ctx) << "native constraints changed frozen PE placement for "
                   << def->getName();
      enqueue(def->getChunk(), index);
    }
  for (const auto &[symbol, name] : roots) {
    auto *def = dyn_cast<Defined>(symbol);
    if (!isa_and_nonnull<DefinedRegular, DefinedCommon>(def) ||
        !def->getChunk())
      Fatal(ctx) << "partition root " << symbol->getName()
                 << " must have a native definition";
    unsigned index = 0;
    for (OutputPartition *output : outputs)
      if (output->name == name)
        index = output->index;
    auto [it, inserted] = fixed.try_emplace(leader(def->getChunk()), index);
    if (!inserted && it->second != index)
      Fatal(ctx) << "indivisible contribution contains roots of different "
                    "partitions: "
                 << symbol->getName();
    enqueue(def->getChunk(), index);
  }
  // Explicit publication pins an identity unless that publication itself is
  // a partition root. Open generated providers are never private placements.
  for (const auto &entry : ctx.symtab.bindingEntities) {
    BindingEntity &entity = *entry.second;
    if (entity.owner != BindingEntity::Published || roots.contains(entry.first))
      continue;
    if (auto *def = dyn_cast<DefinedRegular>(entry.first))
      if (def->data) {
        auto [it, inserted] = fixed.try_emplace(leader(def->getChunk()), 1);
        if (!inserted && it->second != 1)
          Fatal(ctx) << "published owner " << def->getName()
                     << " cannot move into a private partition";
        enqueue(def->getChunk(), 1);
      }
  }
  for (Symbol *symbol : ctx.config.gcroot) {
    if (!roots.contains(symbol)) {
      if (auto *def = dyn_cast_or_null<DefinedRegular>(symbol->getDefined())) {
        if (isImageLocal(def->getChunk())) {
          visit(symbol, 1);
          continue;
        }
        auto [it, inserted] = fixed.try_emplace(leader(def->getChunk()), 1);
        if (!inserted && it->second != 1)
          Fatal(ctx) << "main-image root " << symbol->getName()
                     << " shares an indivisible partition contribution";
      }
      visit(symbol, 1);
    }
  }
  for (ObjFile *file : ctx.objFileInstances) {
    unsigned partition = 0;
    for (const PartitionRoot &root : file->partitionRoots) {
      auto *def = dyn_cast_or_null<DefinedRegular>(root.symbol->getDefined());
      unsigned next =
          def && def->data ? fixed.lookup(leader(def->getChunk())) : 0;
      partition = !partition || partition == next ? next : 1;
    }
    // Non-COMDAT initialization and state follow their input's unambiguous
    // root. Multi-root objects require explicit associative contributions.
    for (Chunk *chunk : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(chunk);
      if (!sc || (sc->live && !sc->isCOMDAT()))
        enqueue(chunk, partition ? partition : 1);
    }
  }
  // Startup participates in the same dependency traversal as application code.
  // Select callbacks before visiting its relocations in each output context.
  drain();
  for (OutputPartition *output : outputs)
    instantiateImageRuntime(*output);
  drain();
  placeSharedPrivateEntities();
  for (Chunk *chunk : ctx.driver.getChunks()) {
    if (auto *sc = dyn_cast<SectionChunk>(chunk))
      if ((!sc->live && !isImageLocal(sc)) || sc != sc->repl)
        continue;
    unsigned partition = owner(chunk);
    if (partition)
      outputs[partition - 1]->chunks.push_back(chunk);
    else if (isImageLocal(chunk))
      for (OutputPartition *output : outputs)
        if (output->imageChunks.contains(chunk))
          output->chunks.push_back(chunk);
  }
  for (MergeChunk *merge : ctx.mergeChunkInstances) {
    if (!merge)
      continue;
    for (SectionChunk *chunk : merge->sections) {
      unsigned partition = owner(chunk);
      if (!chunk->live || !partition)
        continue;
      MergeChunk *&view = outputs[partition - 1]->merges[chunk->p2Align];
      if (!view)
        view = make<MergeChunk>(chunk->getAlignment());
      view->sections.push_back(chunk);
    }
  }
  nameOutputs();
  normalizeExactReferences();
  createViews();
  finalizePrivateImports();
  checkDependencies();
}

static void recordExactExport(DenseMap<ChunkAndOffset, Defined *> &aliases,
                              Symbol *symbol) {
  auto *def = symbol ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                     : nullptr;
  if (!def || !def->data || !isa_and_nonnull<ObjFile>(def->getFile()))
    return;
  auto [it, inserted] =
      aliases.try_emplace({def->getChunk(), def->getValue()}, def);
  if (!inserted && def->getName() < it->second->getName())
    it->second = def;
}

void Partitioning::normalizeExactReferences() {
  // Ownership and GC are final here. Rewrite only surviving cross-output
  // pointer words and address acquisitions; local addresses and original ABI
  // requirements stay intact.
  DenseMap<ChunkAndOffset, Defined *> aliases;
  for (const Export &exp : ctx.symtab.exports)
    if (exp.forwardTo.empty())
      recordExactExport(aliases, exp.sym);
  for (const auto &root : roots)
    recordExactExport(aliases, root.first);
  DenseMap<std::pair<ObjFile *, Defined *>, uint32_t> indices;
  for (OutputPartition *output : outputs)
    for (Chunk *chunk : output->chunks) {
      auto *sc = dyn_cast<SectionChunk>(chunk);
      if (!sc || sc->isCodeView() || sc->isDWARF() || isImageLocal(sc))
        continue;
      sc->sortRelocations();
      ArrayRef<coff_relocation> relocs = sc->getRelocs();
      MutableArrayRef<coff_relocation> replacements;
      uint64_t previousEnd = 0;
      for (auto [i, rel] : enumerate(relocs)) {
        unsigned width = sc->getRelocationWidth(rel);
        bool overlap = width && rel.VirtualAddress < previousEnd;
        if (width)
          previousEnd = std::max(previousEnd,
                                uint64_t(rel.VirtualAddress) + width);
        Symbol *symbol = sc->file->getSymbol(rel.SymbolTableIndex);
        Defined *target = symbol ? symbol->getDefined() : nullptr;
        if (!target || !target->getChunk() || !owner(target->getChunk()) ||
            owner(target->getChunk()) == output->index ||
            isImageLocal(target->getChunk()))
          continue;
        auto offset = getExactDataOffset(*sc, rel, *target);
        if (!offset)
          continue;
        for (size_t next = i + 1;
             next < relocs.size() && uint64_t(relocs[next].VirtualAddress) <
                                         uint64_t(rel.VirtualAddress) + width;
             ++next)
          overlap |= sc->getRelocationWidth(relocs[next]) != 0;
        if (overlap)
          Fatal(ctx) << toString(sc->file) << ": overlapping exact reference at "
                     << sc->getSectionName() << "+" << rel.VirtualAddress;
        auto [it, inserted] = aliases.try_emplace({target->getChunk(), *offset});
        if (inserted) {
          StringRef name = saver().save("__part_exact_" + Twine(aliases.size()));
          auto *alias = make<DefinedSynthetic>(name, target->getChunk(), *offset);
          it->second = alias;
          exactAliases.insert(alias);
        }
        if (replacements.empty()) {
          replacements = {bAlloc().Allocate<coff_relocation>(relocs.size()),
                          relocs.size()};
          llvm::copy(relocs, replacements.begin());
        }
        auto [index, newIndex] = indices.try_emplace({sc->file, it->second});
        if (newIndex)
          index->second = sc->file->addSyntheticSymbol(it->second);
        replacements[i].SymbolTableIndex = index->second;
        ArrayRef<uint8_t> contents = sc->getContents();
        MutableArrayRef<uint8_t> &bytes = sc->file->rewrittenContents[sc->header];
        if (bytes.empty()) {
          bytes = {bAlloc().Allocate<uint8_t>(contents.size()), contents.size()};
          llvm::copy(contents, bytes.begin());
        }
        memset(bytes.data() + rel.VirtualAddress, 0, width);
      }
      if (!replacements.empty())
        sc->setRelocs(replacements);
    }
}

void Partitioning::finalizePrivateImports() {
  DenseMap<Defined *, uint16_t> ordinals;
  for (OutputPartition *output : outputs) {
    SymbolTable &view = output->symbols;
    view.fixupExports();
    view.assignExportOrdinals(/*fillGaps=*/true);
    for (Export &exp : view.exports) {
      // Only linker-created private edges belong to the inseparable set.
      // Public roots and qualified ABI checks retain their named interfaces.
      if (exp.isPrivate && exp.forwardTo.empty()) {
        exp.noname = true;
        ordinals[cast<Defined>(exp.sym)] = exp.ordinal;
      }
    }
  }
  for (auto [file, target] : privateImports)
    if (uint16_t ordinal = ordinals.lookup(target))
      file->setPrivateOrdinal(ordinal);
}

void Partitioning::instantiateImageRuntime(OutputPartition &output) {
  // Only runtime contributions explicitly marked for per-image instantiation
  // may be repeated. Their addresses (including __dso_handle and TLS state)
  // describe this PE, while ordinary C++ objects still have exactly one owner.
  SmallVector<Symbol *, 8> pending;
  bool executable = false;
  bool tls = false;
  for (Chunk *chunk : ctx.driver.getChunks()) {
    if (owner(chunk) != output.index)
      continue;
    executable |= chunk->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE;
    tls |= chunk->getSectionName().starts_with(".tls");
  }
  if (output.index == 1) {
    if (Defined *entry =
            ctx.symtab.entry ? ctx.symtab.entry->getDefined() : nullptr)
      pending.push_back(entry);
  } else if (executable && !ctx.config.noEntry) {
    Symbol *entry = ctx.symtab.findUnderscore("_DllMainCRTStartup");
    Defined *def = entry ? entry->getDefined() : nullptr;
    if (!def || !isImageLocal(def->getChunk()))
      Fatal(ctx) << "executable PE partition requires per-image DLL startup";
    output.symbols.entry = def;
    pending.push_back(def);
    // The user DllMain belongs to the primary image. A generated member has
    // ordinary CRT initialization, without invoking that callback a second
    // time.
    Symbol *callback = ctx.symtab.findUnderscore("DllMain");
    Symbol *fallback = ctx.symtab.findUnderscore("__wincrt_DefaultDllMain");
    if (callback && callback != fallback) {
      Defined *defaultEntry = fallback ? fallback->getDefined() : nullptr;
      if (!defaultEntry || !isImageLocal(defaultEntry->getChunk()))
        Fatal(ctx) << "per-image DLL startup has no default DLL callback";
      output.projections[callback] = defaultEntry;
      pending.push_back(defaultEntry);
    }
  }
  // Native directories are roots even when no instruction refers to them.
  for (StringRef name : {"_tls_used", "_load_config_used"}) {
    if (name == "_tls_used" && !tls)
      continue;
    if (Symbol *symbol = ctx.symtab.findUnderscore(name))
      if (Defined *def = symbol->getDefined())
        if (isImageLocal(def->getChunk()))
          pending.push_back(def);
  }
  for (Symbol *symbol : pending)
    visit(symbol, output.index);
}

void Partitioning::checkDependencies() {
  StringMap<unsigned> indices;
  for (OutputPartition *output : outputs)
    indices[output->dllName.lower()] = output->index;
  SmallVector<uint8_t, 0> state(outputs.size(), 0);
  SmallVector<std::pair<unsigned, size_t>, 16> stack;
  for (unsigned root = 0; root != outputs.size(); ++root) {
    if (state[root])
      continue;
    state[root] = 1;
    stack.emplace_back(root, 0);
    while (!stack.empty()) {
      auto &[index, next] = stack.back();
      OutputPartition &output = *outputs[index];
      if (next == output.imports.size()) {
        state[index] = 2;
        stack.pop_back();
        continue;
      }
      unsigned dependency =
          indices.lookup(StringRef(output.imports[next++]->dllName).lower());
      if (!dependency)
        continue;
      --dependency;
      if (state[dependency] == 1)
        Fatal(ctx) << "cyclic final-output dependency between "
                   << output.dllName << " and " << outputs[dependency]->dllName;
      if (!state[dependency]) {
        state[dependency] = 1;
        stack.emplace_back(dependency, 0);
      }
    }
  }
}

ImportFile *Partitioning::importView(ImportFile *file,
                                     OutputPartition &output) {
  ImportFile *&view = output.importViews[file];
  if (!view) {
    view = file->createView();
    if (output.index == 1)
      view->impSym->isGCRoot = file->impSym->isGCRoot;
    output.imports.push_back(view);
  }
  return view;
}

ImportFile *Partitioning::importDefinition(Defined *symbol,
                                           OutputPartition &output) {
  unsigned index = owner(symbol->getChunk());
  if (!index || index == output.index || isImageLocal(symbol->getChunk()))
    return nullptr;
  OutputPartition &provider = *outputs[index - 1];
  // The full semantic name remains named for externally visible interfaces.
  // Local symbols get deterministic, output-private export spellings.
  StringRef name = provider.exportNames.lookup(symbol);
  auto *regular = dyn_cast<DefinedRegular>(symbol);
  bool isCode =
      symbol->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE;
  if (name.empty()) {
    name = symbol->getName();
    if (regular && !regular->getCOFFSymbol().isExternal())
      name = saver().save("__part_local_" +
                          Twine(provider.symbols.exports.size()));
    Export exp;
    exp.name = exp.exportName = name;
    exp.sym = symbol;
    exp.data = !isCode;
    exp.isPrivate = true;
    exp.source = ExportSource::Linker;
    provider.symbols.exports.push_back(exp);
    provider.exportNames[symbol] = name;
  }
  ImportFile *file =
      ImportFile::create(ctx, symbol->getName(), provider.dllName, name,
                         isCode ? IMPORT_CODE : IMPORT_DATA, false);
  if (isCode)
    file->thunkSym = make<DefinedImportThunk>(
        ctx, symbol->getName(), file->impSym, file->makeImportThunk());
  output.imports.push_back(file);
  privateImports.emplace_back(file, symbol);
  return file;
}

Symbol *Partitioning::project(Symbol *symbol, OutputPartition &output) {
  if (!symbol)
    return nullptr;
  auto it = output.projections.find(symbol);
  if (it != output.projections.end())
    return it->second;
  Symbol *result = symbol;
  if (auto *imp = dyn_cast<DefinedImportData>(symbol)) {
    ImportFile *file = importView(imp->file, output);
    auto *alias = make<DefinedImportData>(imp->getName(), file, file->location);
    alias->isRuntimePseudoReloc = imp->isRuntimePseudoReloc;
    result = alias;
  } else if (auto *thunk = dyn_cast<DefinedImportThunk>(symbol)) {
    result = importView(thunk->wrappedSym->file, output)->thunkSym;
  } else if (auto *local = dyn_cast<DefinedLocalImport>(symbol)) {
    if (local->getChunk()->live) {
      if (ImportFile *file = importDefinition(local, output)) {
        auto *alias =
            make<DefinedImportData>(local->getName(), file, file->location);
        alias->isRuntimePseudoReloc = true;
        result = alias;
      } else {
        output.symbols.localImportChunks.push_back(local->getChunk());
      }
      output.projections[symbol] = result;
      return result;
    }
    auto *target = cast<Defined>(project(local->wrappedSym, output));
    if (auto *thunk = dyn_cast<DefinedImportThunk>(target))
      result = thunk->wrappedSym;
    else if (auto *imp = dyn_cast<DefinedImportData>(target))
      result = imp->file->impSym;
    else {
      auto *view = make<DefinedLocalImport>(ctx, local->getName(), target);
      view->isGCRoot = local->isGCRoot;
      output.symbols.localImports.push_back(view);
      result = view;
    }
  } else if (auto *absolute = dyn_cast<DefinedAbsolute>(symbol)) {
    result = new (make<SymbolUnion>()) DefinedAbsolute(*absolute);
  } else if (auto *synthetic = dyn_cast<DefinedSynthetic>(symbol)) {
    if (exactAliases.contains(synthetic)) {
      if (ImportFile *file = importDefinition(synthetic, output)) {
        auto *alias = make<DefinedImportData>(synthetic->getName(), file,
                                             file->location);
        alias->isRuntimePseudoReloc = true;
        result = alias;
      }
    } else {
      result = new (make<SymbolUnion>()) DefinedSynthetic(*synthetic);
    }
  } else if (auto *def = dyn_cast<Defined>(symbol)) {
    if (isa<DefinedRegular, DefinedCommon>(def)) {
      if (ImportFile *file = importDefinition(def, output)) {
        if (file->thunkSym)
          result = file->thunkSym;
        else {
          auto *alias =
              make<DefinedImportData>(def->getName(), file, file->location);
          alias->isRuntimePseudoReloc = true;
          result = alias;
        }
      }
    }
  }
  output.projections[symbol] = result;
  return result;
}

void Partitioning::createViews() {
  for (OutputPartition *output : outputs) {
    for (const Export &source : ctx.symtab.exports) {
      Export exp = source;
      Defined *def = exp.sym ? exp.sym->getDefined() : nullptr;
      unsigned index = def ? owner(def->getChunk()) : 1;
      if (index == output->index || (!index && output->index == 1)) {
        output->symbols.exports.push_back(exp);
        if (def)
          output->exportNames[def] = exp.exportName;
      }
    }
    for (const auto &[symbol, name] : roots) {
      auto *def = cast<Defined>(symbol);
      if (name != output->name || output->exportNames.contains(def))
        continue;
      Export exp;
      exp.name = exp.exportName = symbol->getName();
      exp.sym = symbol;
      exp.source = ExportSource::Linker;
      exp.data = !(def->getChunk()->getOutputCharacteristics() &
                   IMAGE_SCN_MEM_EXECUTE);
      output->symbols.exports.push_back(exp);
      output->exportNames[def] = exp.exportName;
    }
  }
  // A forwarder is an optional public entry, not an eager import. The main
  // image carries the exact member identity, so GetProcAddress selects the
  // matching private output without a runtime registry or a mutable manifest.
  for (OutputPartition *output : drop_begin(outputs)) {
    for (const Export &source : output->symbols.exports) {
      if (source.isPrivate)
        continue;
      Export exp = source;
      StringRef dll = output->dllName;
      dll.consume_back_insensitive(".dll");
      exp.forwardTo = saver().save(dll + "." + source.exportName);
      outputs.front()->symbols.exports.push_back(exp);
    }
  }
  // Create projections only for surviving edges. A foreign symbol's mere
  // presence in an object's symbol table is not a native dependency.
  for (OutputPartition *output : outputs)
    for (Chunk *chunk : output->chunks)
      if (auto *sc = dyn_cast<SectionChunk>(chunk))
        for (Symbol *symbol : sc->dependencies())
          project(symbol, *output);
  for (OutputPartition *output : outputs) {
    SymbolTable &view = output->symbols;
    for (const auto &entry : ctx.symtab.symMap) {
      Symbol *symbol = entry.second;
      // PE header/mitigation symbols are output-local even when no input
      // relocation currently names them: the writer will fill them later.
      if (isa<DefinedAbsolute, DefinedSynthetic>(symbol))
        project(symbol, *output);
      Symbol *projection = output->projections.lookup(symbol);
      view.symMap.try_emplace(entry.first, projection ? projection : symbol);
    }
    if (output->index == 1)
      view.entry = ctx.symtab.entry;
    for (Export &exp : view.exports)
      if (exp.sym && exp.forwardTo.empty())
        exp.sym = project(exp.sym, *output);
    createImageMetadata(*output);
  }
}

void Partitioning::createImageMetadata(OutputPartition &output) {
  SymbolTable &view = output.symbols;
  // These are PE-local directories and loader-written state. Looking up a
  // foreign definition in the common graph must not put its RVA in this PE's
  // header, or treat an imported pointer cell as the directory itself.
  for (StringRef name :
       {"_tls_used", "_load_config_used", "__security_cookie",
        "__guard_check_icall_fptr", "__guard_dispatch_icall_fptr"}) {
    StringRef mangled = saver().save(view.mangle(name));
    auto *def = dyn_cast_or_null<Defined>(ctx.symtab.find(mangled));
    if (def && def->getChunk() && owner(def->getChunk()) != output.index &&
        !output.imageChunks.contains(def->getChunk()))
      view.symMap.erase(CachedHashStringRef(mangled));
  }
  if (Defined *loadConfig = ctx.symtab.loadConfigSym) {
    if (owner(loadConfig->getChunk()) == output.index ||
        output.imageChunks.contains(loadConfig->getChunk())) {
      view.loadConfigSym = loadConfig;
      view.loadConfigSize = ctx.symtab.loadConfigSize;
      return;
    }
  }
  if (ctx.config.guardCF == GuardCFLevel::Off &&
      !ctx.config.dependentLoadFlags && !ctx.config.safeSEH)
    return;
  auto *loadConfig = make<OutputLoadConfigChunk>(view);
  output.chunks.push_back(loadConfig);
  view.loadConfigSym = cast<Defined>(view.addSynthetic(
      saver().save(view.mangle("_load_config_used")), loadConfig));
  view.loadConfigSize = loadConfig->getSize();
}

void Partitioning::scopeMetadata(ArrayRef<SectionChunk *> chunks,
                                 unsigned index) {
  for (SectionChunk *chunk : chunks) {
    savedLiveness.try_emplace(chunk, chunk->live);
    unsigned partition = owner(chunk);
    // Aggregate symbol-index/debug sections can describe several outputs.
    // Their entries are filtered using the original symbol coordinates.
    chunk->live &= !partition || partition == index;
    chunk->osidx = 0;
  }
}

Symbol *Partitioning::originalSymbol(ObjFile *file, unsigned index) const {
  return originalSymbols.lookup(file)[index];
}

void Partitioning::setDebugView(bool debug) {
  for (SavedFile &saved : savedFiles) {
    auto symbols = saved.file->getMutableSymbols();
    std::copy(saved.symbols.begin(), saved.symbols.end(), symbols.begin());
    if (!debug)
      for (Symbol *&symbol : symbols) {
        auto it = ctx.outputPartition->projections.find(symbol);
        if (it != ctx.outputPartition->projections.end())
          symbol = it->second;
      }
  }
}

void Partitioning::activate(OutputPartition &output) {
  assert(!ctx.outputSymtab && savedFiles.empty());
  ctx.outputSymtab = &output.symbols;
  ctx.outputPartition = &output;
  std::copy(std::begin(ctx.mergeChunkInstances),
            std::end(ctx.mergeChunkInstances), savedMerges.begin());
  std::copy(output.merges.begin(), output.merges.end(),
            std::begin(ctx.mergeChunkInstances));
  for (MergeChunk *merge : savedMerges)
    if (merge)
      for (SectionChunk *chunk : merge->sections) {
        savedLiveness[chunk] = chunk->live;
        chunk->live &= owner(chunk) == output.index;
        chunk->osidx = 0;
      }
  savedImports = std::move(ctx.importFileInstances);
  ctx.importFileInstances = output.imports;
  for (ObjFile *file : ctx.objFileInstances) {
    SavedFile saved{file, {}};
    auto symbols = file->getMutableSymbols();
    saved.symbols.append(symbols.begin(), symbols.end());
    originalSymbols[file] = saved.symbols;
    for (Symbol *&symbol : symbols) {
      auto it = output.projections.find(symbol);
      if (it != output.projections.end())
        symbol = it->second;
      if (symbol) {
        symbol->writtenToSymtab = false;
        if (auto *thunk = dyn_cast<DefinedImportThunk>(symbol))
          thunk->wrappedSym->writtenToSymtab = false;
      }
    }
    savedFiles.push_back(std::move(saved));
    for (Chunk *chunk : file->getChunks()) {
      chunk->osidx = 0;
      if (auto *sc = dyn_cast<SectionChunk>(chunk)) {
        savedLiveness[sc] = sc->live;
        sc->live = output.imageChunks.contains(sc) ||
                   (sc->live && owner(sc) == output.index);
      } else if (auto *common = dyn_cast<CommonChunk>(chunk)) {
        savedCommonLiveness[common] = common->live;
        common->live = output.imageChunks.contains(common) ||
                       (common->live && owner(common) == output.index);
      }
    }
    scopeMetadata(file->getDebugChunks(), output.index);
    scopeMetadata(file->getSXDataChunks(), output.index);
    scopeMetadata(file->getGuardFidChunks(), output.index);
    scopeMetadata(file->getGuardIATChunks(), output.index);
    scopeMetadata(file->getGuardLJmpChunks(), output.index);
    scopeMetadata(file->getGuardEHContChunks(), output.index);
  }
  for (Chunk *chunk : output.chunks)
    if (auto *sc = dyn_cast<SectionChunk>(chunk))
      for (Symbol *symbol : sc->dependencies()) {
        if (auto *imp = dyn_cast_or_null<DefinedImportData>(symbol))
          imp->file->live = true;
        else if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(symbol)) {
          thunk->wrappedSym->file->live = true;
          thunk->getChunk()->live = true;
        }
      }
  for (ImportFile *file : output.imports)
    ctx.config.dllOrder.try_emplace(StringRef(file->dllName).lower(),
                                    ctx.config.dllOrder.size());
  output.symbols.bindLocalImports();
  output.symbols.fixupExports();
  output.symbols.assignExportOrdinals();
}

void Partitioning::deactivate() {
  for (SavedFile &saved : savedFiles) {
    auto symbols = saved.file->getMutableSymbols();
    std::copy(saved.symbols.begin(), saved.symbols.end(), symbols.begin());
  }
  for (const auto &entry : savedLiveness)
    entry.first->live = entry.second;
  for (const auto &entry : savedCommonLiveness)
    entry.first->live = entry.second;
  savedFiles.clear();
  originalSymbols.clear();
  savedLiveness.clear();
  savedCommonLiveness.clear();
  ctx.importFileInstances = std::move(savedImports);
  std::copy(savedMerges.begin(), savedMerges.end(),
            std::begin(ctx.mergeChunkInstances));
  ctx.outputSymtab = nullptr;
  ctx.outputPartition = nullptr;
}

} // namespace lld::coff
