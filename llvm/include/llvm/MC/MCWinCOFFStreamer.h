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
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectStreamer.h"

namespace llvm {

class MCAsmBackend;
class MCContext;
class MCCodeEmitter;
class MCInst;
class MCRegisterInfo;
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
    MovingSections.clear();
    FusedSite.Offset = nullptr;
    Symvers.clear();
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
  void emitELFSymverDirective(const MCSymbol *OriginalSym, StringRef Name,
                              bool KeepOriginalSym) override;
  void emitIdent(StringRef IdentString) override;
  void emitRelocDirective(const MCExpr &Offset, StringRef Name,
                          const MCExpr *Expr, SMLoc Loc = {}) override;
  void emitWinEHHandlerData(SMLoc Loc) override;
  void emitCGProfileEntry(const MCSymbolRefExpr *From,
                          const MCSymbolRefExpr *To, uint64_t Count) override;
  void finishImpl() override;

  /// \}

  /// Returns whether \p Inst, which \p Desc describes, reads the register of
  /// its operand \p RegOp in that operand alone, or as a test in the two it
  /// tests against each other, as a fused site of \p Use does.
  static bool readsFusedRegisterOnce(const MCInst &Inst,
                                     const MCInstrDesc &Desc,
                                     const MCRegisterInfo &MRI,
                                     COFF::FusedUse Use, unsigned RegOp);

protected:
  MCSymbol *CurSymbol;

  /// The symbols that .symver gave a versioned name, which the object has to
  /// define.
  SmallVector<const MCSymbol *, 0> Symvers;

  /// A Windows Itanium or NT-POSIX object's fused relocation types are checked
  /// against its instructions, whose descriptions these are, with what the
  /// target calls the instruction of a fused site, by how it reads its
  /// register.
  std::unique_ptr<const MCInstrInfo> MCII;
  StringRef FusedForms[size_t(COFF::FusedUse::Test) + 1];
  /// What each register unit that the current run wrote holds: the pointer a
  /// load of one read, or null, and whether as a copy of the register that
  /// the load wrote. A copy of what the run did not write holds any pointer.
  /// A run is the instructions of one section since the last that execution
  /// does not fall through.
  DenseMap<unsigned, PointerIntPair<const MCSymbol *, 1, bool>> RunWrites;
  MCSectionSubPair RunSection;
  /// The marker of a fused site, until the instruction of its site is
  /// emitted, with how that reads its register. A jump's relocation is
  /// recorded with its site, whose unwind frame decides its type, and Expr
  /// is the pointer it names.
  /// So is that of a type that names none, whose index its site gives.
  struct {
    const MCExpr *Offset = nullptr;
    const MCSymbol *Pointer;
    SMLoc Loc;
    uint16_t Type;
    COFF::FusedUse Use;
    const MCExpr *Expr;
  } FusedSite;

  /// The instructions still to come of the site at a marker of code that the
  /// linker may resize, and the instructions of a thread-local base's site.
  unsigned SiteInsts = 0;
  unsigned TlsScaleInsts = 0;
  /// The sections with a site after which the linker moves the code, as
  /// COFF::relocationMovesCode says.
  SmallPtrSet<const MCSection *, 0> MovingSections;
  /// Ends the current fragment as code the linker may resize, as an
  /// instruction with a linker-relaxable fixup ends it, so that the assembler
  /// resolves no distance across it. The fragments from \p From on hold the
  /// code where padding came between.
  void endResizableCode(MCFragment *From = nullptr);
  /// A target calls this once it has emitted an instruction, which began in
  /// \p From. \p Resizable says that the linker may resize it or the site it
  /// ends.
  void finishInstruction(bool Resizable, MCFragment *From = nullptr);

  void reportMisplacedFusedSite();
  /// Checks the fused relocation types against \p Inst, which is emitted
  /// next. \p Use says how it reads the register of its operand \p RegOp in
  /// the form of a fused site, as the target describes one, which the pending
  /// marker is to state. \p Load is the pointer it loads whole into the
  /// register it writes, and \p IsRead says that it does as a fused read.
  /// \p Tail says that the linker may
  /// delete a jump there once it has made it direct, which makes a fused jump
  /// a FUSED_JUMP_TAIL.
  /// Returns the label that is the pending marker where the site begins at
  /// \p Inst, which a target may move with the site.
  MCSymbol *checkFused(const MCInst &Inst, COFF::FusedUse Use, unsigned RegOp,
                       const MCSymbol *Load, bool IsRead, bool Tail = false);

  void finalizeCGProfileEntry(const MCSymbolRefExpr *&S);

private:
  void Error(const Twine &Msg) const;
};

} // end namespace llvm

#endif // LLVM_MC_MCWINCOFFSTREAMER_H
