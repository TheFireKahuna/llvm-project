//===- llvm/MC/MCWinCOFFObjectWriter.h - Win COFF Object Writer -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCWINCOFFOBJECTWRITER_H
#define LLVM_MC_MCWINCOFFOBJECTWRITER_H

#include "llvm/MC/MCObjectWriter.h"
#include <memory>
#include <optional>

namespace llvm {

class MCAsmBackend;
class MCContext;
class MCFixup;
class MCValue;
class raw_pwrite_stream;

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

  /// The capabilities field of the link-only records of each object. A
  /// nonzero value gives the object a .llvm_link_records section.
  virtual uint64_t getLinkRecordCapabilities() const { return 0; }

  /// The COFF::LinkSiteForm that the object's link-only records give the
  /// instruction holding \p Fixup, whose relocation in a code section has type
  /// \p Type, or none if the records do not describe it.
  virtual std::optional<unsigned> getLinkSiteForm(const MCFixup &Fixup,
                                                  unsigned Type) const {
    return std::nullopt;
  }
};

class WinCOFFWriter;

class WinCOFFObjectWriter final : public MCObjectWriter {
  friend class WinCOFFWriter;

  std::unique_ptr<MCWinCOFFObjectTargetWriter> TargetObjectWriter;
  std::unique_ptr<WinCOFFWriter> ObjWriter, DwoWriter;
  bool IncrementalLinkerCompatible = false;
  // The capabilities field of the object's link-only records, which the
  // target writer gives. A nonzero value gives the object a
  // .llvm_link_records section.
  uint64_t LinkRecordCapabilities = 0;

public:
  /// A symbol's required address modulo a power of two, which the object's
  /// link-only records carry (COFF::LinkRecordPins).
  struct LinkPin {
    const MCSymbol *Symbol;
    unsigned Log2Modulus;
    uint64_t Residue;
    bool Required;
  };

private:
  // The pins of the object's link-only records, in the order given.
  SmallVector<LinkPin, 0> LinkPins;
  // Whether the object's link-only records say that its KCFI lists name
  // imports by their import address table entries alone.
  bool KCFIImportLists = false;

public:
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
  void setLinkRecordCapabilities(uint64_t Value) {
    LinkRecordCapabilities = Value;
  }
  bool hasLinkRecords() const { return LinkRecordCapabilities != 0; }
  void addLinkPin(const LinkPin &Pin) { LinkPins.push_back(Pin); }
  ArrayRef<LinkPin> getLinkPins() const { return LinkPins; }
  void setKCFIImportLists() { KCFIImportLists = true; }
  bool hasKCFIImportLists() const { return KCFIImportLists; }
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
