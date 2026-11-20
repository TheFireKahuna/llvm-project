//===- llvm/MC/MCWinCOFFObjectWriter.h - Win COFF Object Writer -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCWINCOFFOBJECTWRITER_H
#define LLVM_MC_MCWINCOFFOBJECTWRITER_H

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include <memory>

namespace llvm {

class MCAsmBackend;
class MCContext;
class MCFixup;
class MCValue;
class raw_pwrite_stream;

/// The fixup kind that states COFF relocation type \p Type, as .reloc does.
/// A machine's own types take the first literal kinds. The types from 0x8000
/// up take the last 256, which no ELF type of x86-64 or AArch64 reaches, so
/// that code shared with ELF never takes one of its kinds for one of them.
constexpr MCFixupKind getCOFFRelocFixupKind(unsigned Type) {
  return (Type >> 15 ? FK_NONE - 0x100 : FirstLiteralRelocationKind) +
         (Type & 0xFF);
}

/// The COFF relocation type a literal fixup kind states.
constexpr unsigned getCOFFRelocType(MCFixupKind Kind) {
  return Kind >= FK_NONE - 0x100 ? 0x8000 | (Kind - (FK_NONE - 0x100))
                                 : Kind - FirstLiteralRelocationKind;
}

class MCWinCOFFObjectTargetWriter : public MCObjectTargetWriter {
  virtual void anchor();

  const unsigned Machine;

protected:
  MCWinCOFFObjectTargetWriter(unsigned Machine_);

public:
  ~MCWinCOFFObjectTargetWriter() override = default;

  Triple::ObjectFormatType getFormat() const override { return Triple::COFF; }
  static bool classof(const MCObjectTargetWriter *W) {
    return W->getFormat() == Triple::COFF;
  }

  unsigned getMachine() const { return Machine; }
  virtual unsigned getRelocType(MCContext &Ctx, const MCValue &Target,
                                const MCFixup &Fixup, bool IsCrossSection,
                                const MCAsmBackend &MAB) const = 0;
  virtual bool recordRelocation(const MCFixup &) const { return true; }
};

class WinCOFFWriter;

class WinCOFFObjectWriter final : public MCObjectWriter {
  friend class WinCOFFWriter;

  std::unique_ptr<MCWinCOFFObjectTargetWriter> TargetObjectWriter;
  std::unique_ptr<WinCOFFWriter> ObjWriter, DwoWriter;
  bool IncrementalLinkerCompatible = false;

public:
  /// A fact that the Symbols group of the object's link records states.
  struct LinkFact {
    const MCSymbol *Symbol;
    uint64_t Tag;
    SmallVector<const MCExpr *, 2> Operands;
  };
  std::vector<LinkFact> LinkFacts;
  /// The flags of the object's link records.
  uint64_t LinkRecordsFlags = 0;

  /// A dispatch through a jump table that the Sections group of the object's
  /// link records states: its branch, table, base and load, and the count and
  /// type of the table's entries.
  struct SwitchTable {
    const MCSymbol *Symbols[4];
    uint64_t Count, Type, Flags;
  };
  std::vector<SwitchTable> SwitchTables;

  /// Sections with a value that depends on their layout and that no
  /// relocation states. The link records do not list them as resizable.
  SmallPtrSet<const MCSection *, 0> FixedSizeSections;

  WinCOFFObjectWriter(std::unique_ptr<MCWinCOFFObjectTargetWriter> MOTW,
                      raw_pwrite_stream &OS);
  WinCOFFObjectWriter(std::unique_ptr<MCWinCOFFObjectTargetWriter> MOTW,
                      raw_pwrite_stream &OS, raw_pwrite_stream &DwoOS);

  // MCObjectWriter interface implementation.
  void reset() override;
  void setAssembler(MCAssembler *Asm) override;
  void setIncrementalLinkerCompatible(bool Value) {
    IncrementalLinkerCompatible = Value;
  }
  void executePostLayoutBinding() override;
  bool isSymbolRefDifferenceFullyResolvedImpl(const MCSymbol &SymA,
                                              const MCFragment &FB, bool InSet,
                                              bool IsPCRel) const override;
  void recordRelocation(const MCFragment &F, const MCFixup &Fixup,
                        MCValue Target, uint64_t &FixedValue) override;
  uint64_t writeObject() override;
  int getSectionNumber(const MCSection &Section) const;
};

/// Construct a new Win COFF writer instance.
///
/// \param MOTW - The target specific WinCOFF writer subclass.
/// \param OS - The stream to write to.
/// \returns The constructed object writer.
LLVM_ABI std::unique_ptr<MCObjectWriter>
createWinCOFFObjectWriter(std::unique_ptr<MCWinCOFFObjectTargetWriter> MOTW,
                          raw_pwrite_stream &OS);

LLVM_ABI std::unique_ptr<MCObjectWriter>
createWinCOFFDwoObjectWriter(std::unique_ptr<MCWinCOFFObjectTargetWriter> MOTW,
                             raw_pwrite_stream &OS, raw_pwrite_stream &DwoOS);
} // end namespace llvm

#endif // LLVM_MC_MCWINCOFFOBJECTWRITER_H
