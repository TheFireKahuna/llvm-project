//===- MarkLive.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MarkLive.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "InputFiles.h"
#include "Relocations.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/Timer.h"
#include "llvm/Support/TimeProfiler.h"

namespace lld::coff {

// Set live bit on for each reachable chunk. Unmarked (unreachable)
// COMDAT chunks will be ignored by Writer, so they will be excluded
// from the final output.
void markLive(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Mark live");
  ScopedTimer t(ctx.gcTimer);

  // We build up a worklist of sections which have been marked as live. We only
  // push into the worklist when we discover an unmarked section, and we mark
  // as we push, so sections never appear twice in the list.
  SmallVector<SectionChunk *, 256> worklist;

  // COMDAT section chunks are dead by default. Add non-COMDAT chunks. Do not
  // traverse DWARF sections. They are live, but they should not keep other
  // sections alive.
  // /includecoffsection keeps a section as if it were not a COMDAT.
  for (Chunk *c : ctx.driver.getChunks())
    if (auto *sc = dyn_cast<SectionChunk>(c)) {
      if (ctx.config.includeCoffSections.contains(sc->getSectionName()))
        sc->live = true;
      if (sc->live && !sc->isDWARF())
        worklist.push_back(sc);
    }
  markLive(ctx, ctx.config.gcroot, worklist);
}

void markLive(COFFLinkerContext &ctx, ArrayRef<Symbol *> roots,
              llvm::SmallVectorImpl<SectionChunk *> &worklist) {
  size_t next = 0;
  auto enqueue = [&](SectionChunk *c) {
    if (c->live)
      return;
    c->live = true;
    worklist.push_back(c);
  };

  // A reference to a run's bounds keeps none of its sections, as under ELF's
  // -z start-stop-gc, except for runs named __libc_*: C libraries fill them
  // with entries that nothing else references.
  llvm::DenseMap<Symbol *, ArrayRef<SectionChunk *>> runs;
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    for (SymbolTable::SectionRun &run : symtab.sectionRuns)
      if (run.name.starts_with("__libc_"))
        for (Symbol *bound : {run.start, run.stop})
          if (bound)
            runs[bound] = run.chunks;
  });

  std::function<void(Symbol *)> addSym;

  auto addImportFile = [&](ImportFile *file) {
    file->live = true;
    if (file->impchkThunk && file->impchkThunk->exitThunk)
      addSym(file->impchkThunk->exitThunk);
  };

  addSym = [&](Symbol *s) {
    Defined *b = s->getDefined();
    if (!b)
      return;
    if (auto *sym = dyn_cast<DefinedRegular>(b)) {
      enqueue(sym->getChunk());
    } else if (auto *sym = dyn_cast<DefinedImportData>(b)) {
      addImportFile(sym->file);
      // A call through the pointer may reach the executable's replacement
      // of the import.
      if (sym->file->replacement)
        addSym(sym->file->replacement);
    } else if (auto *sym = dyn_cast<DefinedImportThunk>(b)) {
      addImportFile(sym->wrappedSym->file);
      sym->getChunk()->live = true;
    } else if (auto *sym = dyn_cast<DefinedLocalImport>(b)) {
      // The pointer holds its symbol's address, and a load of it may be
      // rewritten to reach the symbol directly.
      addSym(sym->getTarget());
    } else if (isa<DefinedSynthetic>(b)) {
      for (SectionChunk *c : runs.lookup(b))
        enqueue(c);
      // A thread-local variable's record holds where it and its guard are.
      if (TlsRecordChunk *r = TlsRecordChunk::get(b); r && !r->live) {
        r->live = true;
        addSym(r->var);
        if (r->guard)
          addSym(r->guard);
      }
      // A function override's stub branches to its body or a candidate.
      if (FunctionOverrideThunk *t = FunctionOverrideThunk::get(b);
          t && !t->live) {
        t->live = true;
        addSym(t->fo.body);
        for (Defined *c : t->fo.candidates)
          addSym(c);
      }
    }
  };

  // Add GC root chunks.
  for (Symbol *b : roots)
    addSym(b);

  // A function override with no stub of the link's keeps its candidates live
  // where its default body is.
  auto addCandidates = [&] {
    for (auto [body, fo] : ctx.functionOverrideBodies)
      if (!fo->live && fo->body->getChunk()->live) {
        fo->live = true;
        for (Defined *c : fo->candidates)
          addSym(c);
      }
    return next != worklist.size();
  };

  while (next != worklist.size() || addCandidates()) {
    SectionChunk *sc = worklist[next++];
    assert(sc->live && "We mark as live when pushing onto the worklist!");

    // Mark all symbols listed in the relocation table for this section. Of a
    // function whose address is another image's, only a call or jump reaches
    // the image's copy, and anything else the import of its address.
    // A call of a function with which a DLL offers to replace an overridable
    // default reaches the thunk of the default's import.
    for (auto [b, rel] : zip(sc->symbols(), sc->getRelocs())) {
      DefinedImportData *imp =
          b && b->addressImported
              ? sc->file->symtab.vagueEntities.lookup(b).owner
              : nullptr;
      ImportFile *owner = imp ? imp->file : nullptr;
      if (owner && takesAddress(*sc, rel))
        addImportFile(owner);
      else if (owner && owner->thunkSym && sc->file->forkObject)
        addSym(owner->thunkSym);
      else if (b)
        addSym(b);
    }

    // Mark associative sections if any.
    for (SectionChunk &c : sc->children())
      enqueue(&c);

    // Mark EC entry thunks.
    if (Defined *entryThunk = sc->getEntryThunk())
      addSym(entryThunk);
  }
}
}
