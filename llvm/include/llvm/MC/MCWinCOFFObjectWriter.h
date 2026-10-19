//===- llvm/MC/MCWinCOFFObjectWriter.h - Win COFF Object Writer -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCWINCOFFOBJECTWRITER_H
#define LLVM_MC_MCWINCOFFOBJECTWRITER_H

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCObjectWriter.h"
#include <array>
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
  const uint64_t LinkRecordCapabilities;

public:
  /// A symbol's required address modulo a power of two, which the object's
  /// link-only records carry (COFF::LinkRecordPins).
  struct LinkPin {
    const MCSymbol *Symbol;
    unsigned Log2Modulus;
    uint64_t Residue;
    bool Required;
  };

  /// The primary address point of a vtable, Offset bytes after VTable, with
  /// its class and the class's primary base, which the object's link-only
  /// records carry (COFF::LinkRecordVTables).
  struct LinkVTable {
    const MCSymbol *VTable;
    uint64_t Offset;
    std::string Class;
    std::string Base;
  };

  /// A KCFI thunk, which the object's link-only records describe
  /// (COFF::LinkRecordKCFIThunks).
  struct KCFIThunk {
    const MCSymbol *Thunk;
    unsigned Kind;
    uint32_t Type;
    uint32_t Marker;
    uint32_t Offset;
    const MCSymbol *Mismatch;
  };

  /// The role of an external definition in a COMDAT, which the object's
  /// link-only records carry (COFF::LinkRecordBinding).
  struct LinkBinding {
    const MCSymbol *Symbol;
    unsigned Role;
  };

  /// A forwarder and the import pointer it jumps through, which the object's
  /// link-only records carry (COFF::LinkRecordForwarders).
  struct LinkForwarder {
    const MCSymbol *Forwarder;
    const MCSymbol *Pointer;
  };

  /// An ABI contract that the code in the section of Owner requires of the
  /// image that provides Symbol, which the object's link-only records carry
  /// (COFF::LinkRecordContracts).
  struct LinkContract {
    const MCSymbol *Owner;
    const MCSymbol *Symbol;
    bool IsData;
    std::string Identity;
    std::array<uint8_t, 16> Digest;
  };

  /// A call or jump through the register, numbered as the instruction
  /// encoding numbers it, that the load at Load filled with a pointer, padded
  /// to the length of the same transfer through the pointer in memory, which
  /// the object's link-only records carry (COFF::LinkSiteFusedCall). End is
  /// where the call or jump ends, once a target's streamer has seen it in the
  /// form the records describe.
  struct LinkFusedCall {
    const MCSymbol *End;
    const MCSymbol *Load;
    unsigned Register;
    bool IsJump;
  };

  /// A load of a pointer, the instruction at Site, whose register only fused
  /// calls read, with the end of the instruction that directly follows it, if
  /// known, and whether that one transfers control or may fuse with a branch,
  /// which the object's link-only records carry (COFF::LinkSiteFusedRead).
  struct LinkFusedRead {
    const MCSymbol *Site;
    const MCSymbol *NextEnd;
    bool NextTransfers;
  };

  /// An instruction that reads or writes memory through the register that
  /// the load at Load filled, as the base of its memory operand, which the
  /// object's link-only records carry (COFF::LinkSiteFusedAccess). Start is
  /// where the instruction begins, once a target's streamer has seen it in a
  /// form the records describe, with the offsets in it of its ModRM byte and
  /// of the prefix that holds its base register's high bit (15 for none),
  /// that prefix's kind (a COFF::LinkAccessPrefix), its length, the size of its
  /// displacement (0 for none, 1 for 8 bits, 2 for 32), whether a SIB byte
  /// follows ModRM, and the log2 of the scale of an 8-bit displacement.
  /// Where Index is set, the register is instead added with scale 1, as the
  /// SIB byte's base where SIB is set and its index otherwise, and the load is
  /// a fused access of a thread-local variable's record
  /// (COFF::LinkSiteFusedIndex).
  struct LinkFusedAccess {
    const MCSymbol *Start;
    const MCSymbol *Load;
    uint8_t ModRM;
    uint8_t Prefix;
    uint8_t Length;
    uint8_t Disp;
    bool SIB;
    uint8_t PrefixKind;
    uint8_t Scale;
    bool Index = false;
  };

  /// The instruction after the load at Load to read or write its register,
  /// which adds Constant to it and writes the register numbered Register as
  /// the instruction encoding numbers it, which the object's link-only
  /// records carry (COFF::LinkSiteFusedAdd). Start is where it begins, once a
  /// target's streamer has seen it in a form the records describe.
  struct LinkFusedAdd {
    const MCSymbol *Start;
    const MCSymbol *Load;
    unsigned Register;
    int64_t Constant;
  };

  /// A load of the import pointer of data, the instruction at Site, whose
  /// register only reads of memory read, as their base, which the object's
  /// link-only records carry (COFF::LinkSiteValueOnly), with where the adrp
  /// that gives it the pointer's page begins on ARM64.
  struct LinkValueOnly {
    const MCSymbol *Site;
    const MCSymbol *Page;
  };

  /// A load of the import pointer of data, the instruction at Site, whose
  /// pointer the register numbered Register, as the instruction encoding
  /// numbers it, may give: it holds the pointer that the load at Load loaded,
  /// of other data, which the object's link-only records carry
  /// (COFF::LinkSiteAnchor).
  struct LinkAnchor {
    const MCSymbol *Site;
    const MCSymbol *Load;
    unsigned Register;
  };

  /// A value at Offset in Field that depends on where the code of Target, a
  /// section whose code a linker may resize, lies, which the object's
  /// link-only records carry (COFF::LinkRecordCoverage): A - B + Constant for
  /// a difference, A - Offset + Constant for a PC-relative value, both in a
  /// field of Kind (a COFF::LinkCoverageField) and Size bytes; or, for an
  /// interior entry, the place A in Target to which the relocation at Offset
  /// refers.
  struct LinkCoverageValue {
    const MCSection *Field;
    uint32_t Offset;
    const MCSection *Target;
    uint32_t A;
    uint32_t B;
    int64_t Constant;
    uint8_t Type; // A COFF::LinkCoverageEntry.
    uint8_t Kind;
    uint8_t Size;
  };

private:
  // The pins of the object's link-only records, in the order given.
  SmallVector<LinkPin, 0> LinkPins;
  // The vtables of the object's link-only records, in the order given.
  SmallVector<LinkVTable, 0> LinkVTables;
  // The kinds of the object's link-only records with an empty payload, each
  // stating a fact about the whole object, in increasing order.
  SmallVector<uint64_t, 0> LinkFacts;
  // The KCFI thunks of the object's link-only records, in the order given.
  SmallVector<KCFIThunk, 0> KCFIThunks;
  // The KCFI prefixes whose word before the marker is a membership tag.
  SmallVector<const MCSymbol *, 0> KCFIMemberTags;
  // The roles of the object's definitions in COMDATs, in the order given.
  SmallVector<LinkBinding, 0> LinkBindings;
  // The object's forwarders, in the order given.
  SmallVector<LinkForwarder, 0> LinkForwarders;
  // The ABI contracts the object's references require, in the order given.
  SmallVector<LinkContract, 0> LinkContracts;
  // The facts about the object's fused calls and the loads they read.
  SmallVector<LinkFusedCall, 0> LinkFusedCalls;
  SmallVector<LinkFusedRead, 0> LinkFusedReads;
  SmallVector<LinkFusedAccess, 0> LinkFusedAccesses;
  SmallVector<LinkFusedAdd, 0> LinkFusedAdds;
  SmallVector<LinkValueOnly, 0> LinkValueOnlyLoads;
  SmallVector<LinkAnchor, 0> LinkAnchors;
  // Where the jumps begin that end their functions, which the object's
  // link-only records list (COFF::LinkRecordTailJumps) where each ends its
  // section too.
  SmallVector<const MCSymbol *, 0> LinkTailJumps;
  // The values that depend on where code a linker may resize lies.
  SmallVector<LinkCoverageValue, 0> LinkCoverageValues;

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
  bool hasLinkRecords() const { return LinkRecordCapabilities != 0; }
  // Whether the object has a .llvm_link_records section.
  bool hasLinkRecordsSection() const {
    return hasLinkRecords() || !LinkPins.empty() || !LinkFacts.empty() ||
           !KCFIThunks.empty() || !KCFIMemberTags.empty() ||
           !LinkBindings.empty() || !LinkForwarders.empty() ||
           !LinkContracts.empty() || !LinkVTables.empty();
  }
  void addLinkPin(const LinkPin &Pin) { LinkPins.push_back(Pin); }
  void addLinkVTable(LinkVTable VTable) {
    LinkVTables.push_back(std::move(VTable));
  }
  void addLinkFact(uint64_t Kind) {
    auto I = llvm::lower_bound(LinkFacts, Kind);
    if (I == LinkFacts.end() || *I != Kind)
      LinkFacts.insert(I, Kind);
  }
  void addKCFIThunk(const KCFIThunk &Thunk) { KCFIThunks.push_back(Thunk); }
  void addKCFIMemberTag(const MCSymbol *Prefix) {
    KCFIMemberTags.push_back(Prefix);
  }
  void addLinkForwarder(const LinkForwarder &Forwarder) {
    LinkForwarders.push_back(Forwarder);
  }
  void addLinkContract(const MCSymbol *Owner, const MCSymbol *Symbol,
                       bool IsData, StringRef Identity,
                       ArrayRef<uint8_t> Digest) {
    assert(Digest.size() == 16 && "a contract's digest has 16 bytes");
    LinkContract &C = LinkContracts.emplace_back();
    C.Owner = Owner;
    C.Symbol = Symbol;
    C.IsData = IsData;
    C.Identity = Identity.str();
    llvm::copy(Digest, C.Digest.begin());
  }
  void addLinkFusedCall(const LinkFusedCall &Call) {
    LinkFusedCalls.push_back(Call);
  }
  void addLinkFusedRead(const LinkFusedRead &Read) {
    LinkFusedReads.push_back(Read);
  }
  void addLinkFusedAccess(const LinkFusedAccess &Access) {
    LinkFusedAccesses.push_back(Access);
  }
  void addLinkFusedAdd(const LinkFusedAdd &Add) {
    LinkFusedAdds.push_back(Add);
  }
  void addLinkValueOnly(const LinkValueOnly &Load) {
    LinkValueOnlyLoads.push_back(Load);
  }
  void addLinkAnchor(const LinkAnchor &Anchor) { LinkAnchors.push_back(Anchor); }
  void addLinkTailJump(const MCSymbol *Site) { LinkTailJumps.push_back(Site); }
  void addLinkCoverageValue(const LinkCoverageValue &Value) {
    LinkCoverageValues.push_back(Value);
  }
  /// The fused call and read added last, which a target's streamer completes
  /// once it has seen the call, or the instruction that follows the load.
  LinkFusedCall &getLastLinkFusedCall() { return LinkFusedCalls.back(); }
  LinkFusedRead &getLastLinkFusedRead() { return LinkFusedReads.back(); }
  /// The fused access and add added last, which a target's streamer completes
  /// once it has seen the instruction.
  LinkFusedAccess &getLastLinkFusedAccess() { return LinkFusedAccesses.back(); }
  LinkFusedAdd &getLastLinkFusedAdd() { return LinkFusedAdds.back(); }
  // A binding implies the fact that the object lists every binding.
  void addLinkBinding(const LinkBinding &Binding) {
    LinkBindings.push_back(Binding);
    addLinkFact(COFF::LinkRecordBinding);
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
