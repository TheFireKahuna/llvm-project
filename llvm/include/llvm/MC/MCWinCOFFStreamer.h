//===- MCWinCOFFStreamer.h - COFF Object File Interface ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCWINCOFFSTREAMER_H
#define LLVM_MC_MCWINCOFFSTREAMER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectStreamer.h"

namespace llvm {

class MCAsmBackend;
class MCContext;
class MCCodeEmitter;
class MCInst;
class MCSection;
class MCSubtargetInfo;
class MCSymbol;
class StringRef;
class WinCOFFObjectWriter;
class raw_pwrite_stream;

class LLVM_ABI MCWinCOFFStreamer : public MCObjectStreamer {
public:
  MCWinCOFFStreamer(MCContext &Context, std::unique_ptr<MCAsmBackend> MAB,
                    std::unique_ptr<MCCodeEmitter> CE,
                    std::unique_ptr<MCObjectWriter> OW);

  /// state management
  void reset() override {
    CurSymbol = nullptr;
    RunWrites.clear();
    FusedSite.Offset = nullptr;
    MCObjectStreamer::reset();
  }

  WinCOFFObjectWriter &getWriter();

  /// \name MCStreamer interface
  /// \{

  void initSections(const MCSubtargetInfo &STI) override;
  void changeSection(MCSection *Section, uint32_t Subsection = 0) override;
  void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc()) override;
  bool emitSymbolAttribute(MCSymbol *Symbol, MCSymbolAttr Attribute) override;
  void emitSymbolDesc(MCSymbol *Symbol, unsigned DescValue) override;
  void beginCOFFSymbolDef(MCSymbol const *Symbol) override;
  void emitCOFFSymbolStorageClass(int StorageClass) override;
  void emitCOFFSymbolType(int Type) override;
  void endCOFFSymbolDef() override;
  void emitCOFFSafeSEH(MCSymbol const *Symbol) override;
  void emitELFSize(MCSymbol *Symbol, const MCExpr *Value) override;
  void emitCOFFLinkFact(const MCSymbol *Symbol, uint64_t Tag,
                        ArrayRef<const MCExpr *> Operands) override;
  void emitCOFFSwitchTable(const MCSymbol *Branch, const MCSymbol *Table,
                           const MCSymbol *Base, uint64_t Count, uint64_t Type,
                           const MCSymbol *Load, uint64_t Flags) override;
  void emitCOFFSymbolIndex(MCSymbol const *Symbol) override;
  void emitCOFFSectionIndex(MCSymbol const *Symbol) override;
  void emitCOFFSecRel32(MCSymbol const *Symbol, uint64_t Offset) override;
  void emitCOFFImgRel32(MCSymbol const *Symbol, int64_t Offset) override;
  void emitCOFFSecNumber(MCSymbol const *Symbol) override;
  void emitCOFFSecOffset(MCSymbol const *Symbol) override;
  void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                        Align ByteAlignment) override;
  void emitLocalCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                             Align ByteAlignment) override;
  void emitWeakReference(MCSymbol *Alias, const MCSymbol *Symbol) override;
  void emitIdent(StringRef IdentString) override;
  void emitRelocDirective(const MCExpr &Offset, StringRef Name,
                          const MCExpr *Expr, SMLoc Loc = {}) override;
  void emitWinEHHandlerData(SMLoc Loc) override;
  void emitCGProfileEntry(const MCSymbolRefExpr *From,
                          const MCSymbolRefExpr *To, uint64_t Count) override;
  void finishImpl() override;

  /// \}

protected:
  MCSymbol *CurSymbol;

  /// A Windows Itanium or NT-POSIX object's fused relocation types are checked
  /// against its instructions, whose descriptions these are, with what the
  /// target calls the instruction of a fused call and of a fused jump.
  std::unique_ptr<const MCInstrInfo> MCII;
  StringRef FusedForms[2];
  /// What each register unit that the current run wrote holds: the pointer a
  /// fused read loaded, or null. A run is the instructions of one section
  /// since the last that execution does not fall through.
  DenseMap<unsigned, const MCSymbol *> RunWrites;
  MCSectionSubPair RunSection;
  /// The marker of a fused call or jump, until the instruction of its site is
  /// emitted. Placed says that the site began at the marker.
  struct {
    const MCExpr *Offset = nullptr;
    const MCSymbol *Pointer;
    SMLoc Loc;
    bool IsJump;
    bool Placed;
  } FusedSite;

  void reportMisplacedFusedSite();
  /// Checks the fused relocation types against \p Inst, which is emitted
  /// next. \p Read is the pointer it loads as a fused read. \p IsSite says
  /// that it is the call or the jump of the pending marker's form, through its
  /// first operand, and \p IsPad that it is a part of that form before it.
  /// Returns the label that is the pending marker where the site begins at
  /// \p Inst, which a target may move with the site.
  MCSymbol *checkFused(const MCInst &Inst, const MCSymbol *Read, bool IsSite,
                       bool IsPad = false);

  void finalizeCGProfileEntry(const MCSymbolRefExpr *&S);

private:
  void Error(const Twine &Msg) const;
};

} // end namespace llvm

#endif // LLVM_MC_MCWINCOFFSTREAMER_H
