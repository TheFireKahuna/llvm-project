//===- LTOPartitions.cpp - Resolve PE placement before optimization
//---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Binding.h"
#include "COFFLinkerContext.h"
#include "Partitions.h"
#include "Symbols.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Support/SHA256.h"

using namespace llvm;
using namespace lld;
using namespace lld::coff;

namespace {
struct InputModule {
  BitcodeFile *file;
  std::unique_ptr<Module> module;
  bool thin;
  DenseMap<const GlobalValue *, Symbol *> symbols;
};

struct PlacementNode {
  SmallVector<unsigned, 4> dependencies;
  unsigned fixed = 0;
  unsigned owner = 0;
  StringRef external;
  bool metadata = false;
  bool imageLocal = false;
  bool localStorage = false;
};

// Symbols join IR definitions and native contributions in the same resolution
// graph. Module-local values have their own nodes until a cross-output edge
// requires promotion. COMDAT/lifetime and opaque native edges are indivisible.
class LTOPlacement {
  COFFLinkerContext &ctx;
  Partitioning &partitions;
  LLVMContext context;
  std::vector<InputModule> modules;
  DenseMap<const void *, unsigned> indices;
  DenseMap<DefinedImportData *, unsigned> importTargets;
  DenseSet<unsigned> importCells;
  std::vector<PlacementNode> nodes;
  EquivalenceClasses<unsigned> groups;
  DenseMap<const GlobalValue *, unsigned> values;
  StringMap<unsigned> names;
  unsigned sharedMetadata = 0;

  unsigned node(const void *key) {
    auto [it, inserted] = indices.try_emplace(key, nodes.size());
    if (inserted) {
      nodes.emplace_back();
      groups.insert(it->second);
    }
    return it->second;
  }

  unsigned symbolNode(Symbol *symbol) {
    symbol = getBindingTarget(symbol);
    if (isa<DefinedSynthetic, DefinedAbsolute>(symbol)) {
      unsigned index = node(symbol);
      nodes[index].imageLocal = true;
      return index;
    }
    if (auto *thunk = dyn_cast<DefinedImportThunk>(symbol))
      return importTarget(thunk->wrappedSym);
    if (auto *import = dyn_cast<DefinedImportData>(symbol)) {
      if (import->isRuntimePseudoReloc)
        return importTarget(import);
      unsigned index = node(symbol);
      importCells.insert(index);
      return index;
    }
    if (auto *regular = dyn_cast<DefinedRegular>(symbol))
      if (isa_and_nonnull<ObjFile>(regular->getFile()) && regular->data) {
        unsigned index = node(regular->getChunk());
        nodes[index].imageLocal |= partitions.isImageLocal(regular->getChunk());
        return index;
      }
    if (auto *common = dyn_cast<DefinedCommon>(symbol))
      return node(common->getChunk());
    return node(symbol);
  }

  unsigned importTarget(DefinedImportData *import) {
    auto [it, inserted] =
        importTargets.try_emplace(import->file->impSym, nodes.size());
    if (inserted) {
      nodes.emplace_back();
      groups.insert(it->second);
      nodes.back().external = saver().save("dll:" + import->getDLLName());
    }
    return it->second;
  }

  void pin(unsigned index, unsigned output) {
    if (nodes[index].fixed && nodes[index].fixed != output)
      Fatal(ctx) << "conflicting final PE assignments before LTO";
    nodes[index].fixed = output;
  }

  void collectConstants(unsigned from, ArrayRef<Constant *> constants,
                        bool initializer) {
    SmallPtrSet<Constant *, 32> seen;
    SmallVector<Constant *, 16> pending(constants);
    while (!pending.empty()) {
      Constant *c = pending.pop_back_val();
      if (!seen.insert(c).second)
        continue;
      if (auto *gv = dyn_cast<GlobalValue>(c)) {
        auto it = values.find(gv);
        if (it == values.end())
          continue;
        nodes[from].dependencies.push_back(it->second);
        if (!nodes[it->second].imageLocal &&
            (gv->isThreadLocal() || importCells.contains(it->second)))
          groups.unionSets(from, it->second);
        continue;
      }
      if (auto *address = dyn_cast<BlockAddress>(c)) {
        groups.unionSets(from, values.lookup(address->getFunction()));
        continue;
      }
      // A bounded constant data address can use a demanded exact export after
      // final placement. Other interiors retain their co-location constraint.
      bool exactData = false;
      if (initializer && ctx.config.importSlots)
        if (auto *gep = dyn_cast<GEPOperator>(c))
          if (auto *variable = dyn_cast<GlobalVariable>(
                  gep->getPointerOperand()->stripPointerCasts())) {
            const DataLayout &layout = variable->getDataLayout();
            APInt offset(layout.getIndexTypeSizeInBits(gep->getType()), 0);
            if (variable->getValueType()->isSized()) {
              TypeSize size = layout.getTypeAllocSize(variable->getValueType());
              exactData = !variable->isThreadLocal() && !size.isScalable() &&
                          gep->accumulateConstantOffset(layout, offset) &&
                          !offset.isNegative() && offset.getActiveBits() <= 64 &&
                          offset.getZExtValue() < size.getFixedValue();
            }
          }
      if (initializer && !exactData)
        if (auto *expression = dyn_cast<ConstantExpr>(c))
          if (expression->getOpcode() == Instruction::GetElementPtr)
            for (const Use &use : expression->operands())
              if (auto *gv = dyn_cast<GlobalValue>(use.get()))
                if (auto it = values.find(gv); it != values.end())
                  if (!nodes[it->second].imageLocal &&
                      nodes[it->second].external.empty())
                    groups.unionSets(from, it->second);
      for (Value *operand : c->operand_values())
        if (auto *constant = dyn_cast<Constant>(operand))
          pending.push_back(constant);
    }
  }

  void readModules() {
    for (BitcodeFile *file : ctx.symtab.bitcodeFileInstances) {
      auto &bitcode = file->obj->getSingleBitcodeModule();
      auto info = check(bitcode.getLTOInfo());
      InputModule input{file, check(bitcode.parseModule(context)),
                        info.IsThinLTO,
                        DenseMap<const GlobalValue *, Symbol *>()};
      for (auto [record, symbol] :
           zip(file->obj->symbols(), file->getSymbols()))
        if (GlobalValue *gv = input.module->getNamedValue(record.getIRName()))
          input.symbols[gv] = symbol;
      for (GlobalValue &gv : input.module->global_values()) {
        Symbol *symbol = input.symbols.lookup(&gv);
        Symbol *semantic = symbol;
        auto *import =
            symbol ? dyn_cast<DefinedImportData>(getBindingTarget(symbol))
                   : nullptr;
        if (symbol && gv.hasDLLImportStorageClass()) {
          StringRef name = symbol->getName();
          if (name.consume_front("__imp_"))
            if (Symbol *pointee = ctx.symtab.find(name))
              semantic = pointee;
        }
        unsigned index = import && gv.hasDLLImportStorageClass()
                             ? importTarget(import)
                         : semantic ? symbolNode(semantic)
                                    : node(&gv);
        values[&gv] = index;
        nodes[index].imageLocal |=
            file->imageLocal && !gv.isDeclarationForLinker();
        if (file->imageLocal && symbol && !gv.isDeclarationForLinker())
          symbol->isUsedInRegularObj = true;
        if (!symbol)
          continue;
        auto *defined = dyn_cast<DefinedRegular>(getBindingTarget(symbol));
        if (!import)
          import = ctx.symtab.getBindingImport(symbol);
        if (import && !importCells.contains(index) &&
            (!defined || defined->isCOMDAT))
          nodes[index].external = saver().save("dll:" + import->getDLLName());
        BindingEntity *entity =
            ctx.symtab.bindingEntities.lookup(getBindingTarget(symbol));
        if (entity && entity->owner == BindingEntity::Published)
          pin(index, 1);
        if (entity && entity->owner == BindingEntity::Private)
          nodes[index].metadata = true;
      }
      modules.push_back(std::move(input));
    }
  }

  void collectIR() {
    for (InputModule &input : modules) {
      Module &module = *input.module;
      DenseMap<const Comdat *, unsigned> comdats;
      for (GlobalValue &gv : module.global_values()) {
        if (gv.getName().starts_with("llvm."))
          continue;
        unsigned index = values.lookup(&gv);
        if (gv.hasPartition() && !gv.isDeclarationForLinker())
          for (OutputPartition *output : partitions.outputs)
            if (output->name == gv.getPartition())
              pin(index, output->index);
        if (Symbol *symbol = input.symbols.lookup(&gv))
          if (!gv.isDeclarationForLinker() &&
              getBindingTarget(symbol)->getFile() != input.file)
            continue;
        nodes[index].localStorage |= !gv.isDeclarationForLinker() &&
                                     nodes[index].external.empty() &&
                                     !nodes[index].imageLocal;
        if (auto *object = dyn_cast<GlobalObject>(&gv))
          if (MDNode *uses = object->getMetadata("coff.abi.uses"))
            for (const MDOperand &operand : uses->operands()) {
              auto *use = dyn_cast_or_null<MDNode>(operand);
              auto *value = use && use->getNumOperands() == 2
                                ? dyn_cast_or_null<ValueAsMetadata>(use->getOperand(0))
                                : nullptr;
              auto *target = value ? dyn_cast<GlobalValue>(value->getValue()) : nullptr;
              if (!target || !values.contains(target) ||
                  !isa_and_nonnull<MDString>(use->getOperand(1)))
                Fatal(ctx) << toString(input.file) << ": invalid COFF ABI use metadata";
              nodes[index].dependencies.push_back(values.lookup(target));
            }
        if (auto *object = dyn_cast<GlobalObject>(&gv))
          if (const Comdat *comdat = object->getComdat()) {
            auto [it, inserted] = comdats.try_emplace(comdat, index);
            if (!inserted)
              groups.unionSets(index, it->second);
          }
        if (auto *alias = dyn_cast<GlobalAlias>(&gv)) {
          if (GlobalObject *target = alias->getAliaseeObject())
            groups.unionSets(index, values.lookup(target));
        } else if (auto *variable = dyn_cast<GlobalVariable>(&gv)) {
          if (variable->hasInitializer())
            collectConstants(index, {variable->getInitializer()}, true);
        } else if (auto *function = dyn_cast<Function>(&gv)) {
          SmallVector<Constant *, 16> constants;
          for (Instruction &instruction : instructions(function))
            for (Value *operand : instruction.operand_values())
              if (auto *constant = dyn_cast<Constant>(operand))
                constants.push_back(constant);
          if (function->hasPersonalityFn())
            constants.push_back(function->getPersonalityFn());
          collectConstants(index, constants, false);
        }
      }
    }
  }

  void collectNative() {
    for (ObjFile *file : ctx.objFileInstances) {
      for (Chunk *chunk : file->getChunks()) {
        auto *section = dyn_cast<SectionChunk>(chunk);
        if (!section || section->isCodeView() || section->isDWARF())
          continue;
        unsigned from = node(section);
        nodes[from].imageLocal |= partitions.isImageLocal(section);
        nodes[from].localStorage |=
            !nodes[from].imageLocal &&
            (!section->sym || !ctx.symtab.getBindingImport(section->sym));
        for (SectionChunk &child : section->children())
          groups.unionSets(from, node(&child));
        for (uint32_t index : file->getABIUses(section))
          nodes[from].dependencies.push_back(symbolNode(file->getSymbol(index)));
        for (const auto &relocation : section->getRelocs()) {
          Symbol *symbol = file->getSymbol(relocation.SymbolTableIndex);
          if (!symbol)
            continue;
          unsigned to = symbolNode(symbol);
          auto *import = dyn_cast<DefinedImportData>(symbol->getDefined());
          if (import && !import->isRuntimePseudoReloc) {
            auto form = section->getImportRefForm(relocation);
            if (form != SectionChunk::ImportRefForm::None &&
                form != SectionChunk::ImportRefForm::Lea)
              to = importTarget(import);
            else
              groups.unionSets(from, to);
          }
          nodes[from].dependencies.push_back(to);
          Defined *target = symbol->getDefined();
          if (!nodes[from].imageLocal && !nodes[to].imageLocal && target &&
              isa<DefinedRegular, DefinedCommon>(target) &&
              !canImportPartitionReference(*section, relocation, *target))
            groups.unionSets(from, to);
        }
      }
    }
  }

  void solve() {
    for (OutputPartition *output : partitions.outputs)
      names[output->name] = output->index;
    for (ObjFile *file : ctx.objFileInstances)
      for (const PartitionRoot &root : file->partitionRoots)
        if (partitions.isRoot(root.symbol->getDefined()))
          pin(symbolNode(root.symbol), names.lookup(root.name));
    for (BitcodeFile *file : ctx.symtab.bitcodeFileInstances)
      for (const PartitionRoot &root : file->partitionRoots)
        if (partitions.isRoot(root.symbol->getDefined()))
          pin(symbolNode(root.symbol), names.lookup(root.name));
    for (Symbol *root : ctx.config.gcroot)
      if (!partitions.isRoot(root->getDefined()) &&
          !nodes[symbolNode(root)].imageLocal)
        pin(symbolNode(root), 1);
    for (unsigned i = 0; i != nodes.size(); ++i) {
      unsigned leader = groups.getLeaderValue(i);
      if (nodes[i].fixed)
        pin(leader, nodes[i].fixed);
      nodes[leader].metadata |= nodes[i].metadata;
      nodes[leader].imageLocal |= nodes[i].imageLocal;
      nodes[leader].localStorage |= nodes[i].localStorage;
      if (!nodes[i].external.empty()) {
        if (!nodes[leader].external.empty() &&
            nodes[leader].external != nodes[i].external)
          Fatal(ctx)
              << "indivisible PE contribution selects different providers";
        nodes[leader].external = nodes[i].external;
      }
      if (leader != i)
        append_range(nodes[leader].dependencies, nodes[i].dependencies);
    }
    for (const PlacementNode &entry : nodes)
      if (entry.localStorage && !entry.external.empty())
        Fatal(ctx) << "indivisible PE contribution requires local storage in "
                      "an independently linked provider";
    SmallVector<unsigned, 32> pending;
    for (unsigned i = 0; i != nodes.size(); ++i)
      if (nodes[i].fixed) {
        nodes[i].owner = nodes[i].fixed;
        pending.push_back(i);
      }
    while (!pending.empty()) {
      unsigned from = pending.pop_back_val();
      for (unsigned dependency : nodes[from].dependencies) {
        unsigned to = groups.getLeaderValue(dependency);
        PlacementNode &target = nodes[to];
        if (target.fixed || target.imageLocal || !target.external.empty())
          continue;
        unsigned next = target.owner && target.owner != nodes[from].owner
                            ? 1
                            : nodes[from].owner;
        if (target.metadata && target.owner &&
            target.owner != nodes[from].owner) {
          if (!sharedMetadata) {
            StringRef name = ".llvm.rtti";
            unsigned suffix = 0;
            while (names.contains(name))
              name = saver().save(".llvm.rtti." + Twine(++suffix));
            sharedMetadata = partitions.outputs.size() + 1;
            partitions.outputs.push_back(
                make<OutputPartition>(ctx, name, sharedMetadata));
            names[name] = sharedMetadata;
          }
          next = sharedMetadata;
        }
        if (next != target.owner) {
          target.owner = next;
          pending.push_back(to);
        }
      }
    }
  }

  StringRef owner(unsigned index) {
    PlacementNode &entry = nodes[groups.getLeaderValue(index)];
    if (entry.imageLocal)
      return "image:";
    if (!entry.external.empty())
      return entry.external;
    unsigned output = entry.owner;
    return output ? partitions.outputs[output - 1]->name : StringRef();
  }

  void annotate() {
    DenseSet<unsigned> foreignTargets;
    for (unsigned from = 0; from != nodes.size(); ++from)
      for (unsigned to : nodes[from].dependencies)
        if (owner(from) != owner(to) && owner(to) != "image:")
          foreignTargets.insert(to);
    for (InputModule &input : modules) {
      Module &module = *input.module;
      // A FullLTO backend can contain both runtime templates and user code.
      // Replace the input-wide directive with per-definition placements.
      if (input.file->imageLocal)
        if (NamedMDNode *options =
                module.getNamedMetadata("llvm.linker.options")) {
          SmallVector<MDNode *, 8> retained;
          for (MDNode *option : options->operands()) {
            SmallVector<Metadata *, 2> arguments;
            for (const MDOperand &operand : option->operands()) {
              auto *text = dyn_cast<MDString>(operand);
              if (!text ||
                  !text->getString().equals_insensitive("/lldimagelocal"))
                arguments.push_back(operand);
            }
            if (!arguments.empty())
              retained.push_back(MDNode::get(context, arguments));
          }
          options->clearOperands();
          for (MDNode *option : retained)
            options->addOperand(option);
        }
      module.addModuleFlag(Module::Error, "coff.output-set", 1);
      if (ctx.config.importSlots)
        module.addModuleFlag(Module::Error, "coff.import-slots", 1);
      auto moduleHash =
          SHA256::hash(arrayRefFromStringRef(input.file->mb.getBuffer()));
      for (GlobalValue &gv : module.global_values()) {
        if (gv.getName().starts_with("llvm."))
          continue;
        bool foreign = foreignTargets.contains(values.lookup(&gv));
        if (gv.hasLocalLinkage() && foreign) {
          SHA256 hash;
          hash.update(moduleHash);
          hash.update(gv.getName());
          std::string name = "__llvm_part_" + toHex(hash.final(), true);
          if (auto *object = dyn_cast<GlobalObject>(&gv))
            if (Comdat *old = object->getComdat())
              if (old->getName() == gv.getName()) {
                Comdat *replacement = module.getOrInsertComdat(name);
                replacement->setSelectionKind(old->getSelectionKind());
                for (GlobalObject &member : module.global_objects())
                  if (member.getComdat() == old)
                    member.setComdat(replacement);
              }
          gv.setName(name);
          gv.setLinkage(GlobalValue::ExternalLinkage);
          gv.setVisibility(GlobalValue::HiddenVisibility);
        }
        StringRef output = owner(values.lookup(&gv));
        gv.setPartition(output.starts_with("dll:") || output == "image:"
                            ? output
                            : saver().save("pe:" + output));
        // A combined module has no single image-relative locality. The late
        // lowering restores direct local uses after inlining has settled.
        if (!gv.hasLocalLinkage())
          gv.setDSOLocal(false);
        if (foreign)
          if (Symbol *symbol = input.symbols.lookup(&gv))
            symbol->isUsedInRegularObj = true;
        // The CRT callback is projected independently in each image. Keep
        // that call explicit through LTO; inlining a primary-image DllMain
        // into reusable startup would run it again in every generated DLL.
        if (output == "image:")
          if (auto *function = dyn_cast<Function>(&gv))
            for (Instruction &instruction : instructions(function))
              if (auto *call = dyn_cast<CallBase>(&instruction))
                if (Function *callee = call->getCalledFunction())
                  if (callee->getName() == "DllMain")
                    call->addFnAttr(Attribute::NoInline);
      }
      SmallVector<char, 0> bytes;
      raw_svector_ostream stream(bytes);
      std::optional<ModuleSummaryIndex> summary;
      if (input.thin) {
        ProfileSummaryInfo profile(module);
        summary.emplace(buildModuleSummaryIndex(module, nullptr, &profile));
      }
      WriteBitcodeToFile(module, stream, true, summary ? &*summary : nullptr,
                         /*GenerateHash=*/true);
      MemoryBufferRef buffer =
          ctx.driver.takeBuffer(MemoryBuffer::getMemBufferCopy(
              stream.str(), input.file->mb.getBufferIdentifier()));
      input.file->replaceLTOObject(check(lto::InputFile::create(buffer)));
    }
    // Native members participated in the same solve. Freeze their placements
    // in memory; they need no rewritten object or additional object records.
    for (Chunk *chunk : ctx.driver.getChunks())
      if (auto it = indices.find(chunk); it != indices.end())
        if (!owner(it->second).starts_with("dll:") &&
            owner(it->second) != "image:")
          partitions.freeze(chunk, owner(it->second));
  }

public:
  LTOPlacement(COFFLinkerContext &ctx, Partitioning &partitions)
      : ctx(ctx), partitions(partitions) {}
  void run() {
    readModules();
    collectIR();
    collectNative();
    solve();
    annotate();
  }
};
} // namespace

void Partitioning::prepareLTO() {
  if (ctx.symtab.bitcodeFileInstances.empty())
    return;
  if (empty()) {
    if (ctx.symtab.bindingEntities.empty())
      return;
    outputs.push_back(make<OutputPartition>(ctx, "", 1));
  }
  LTOPlacement(ctx, *this).run();
}
