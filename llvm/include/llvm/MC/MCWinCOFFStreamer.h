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
    FusionUnits.clear();
    FusionLabel = false;
    FusionLoads.clear();
    FusionChecks.clear();
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
  void emitCOFFSymbolIndex(MCSymbol const *Symbol) override;
  void emitCOFFSectionIndex(MCSymbol const *Symbol) override;
  void emitCOFFSecRel32(MCSymbol const *Symbol, uint64_t Offset) override;
  void emitCOFFImgRel32(MCSymbol const *Symbol, int64_t Offset) override;
  void emitCOFFSecNumber(MCSymbol const *Symbol) override;
  void emitCOFFSecOffset(MCSymbol const *Symbol) override;
  void emitCOFFLinkPin(MCSymbol const *Symbol, unsigned Log2Modulus,
                       uint64_t Residue, bool Required) override;
  void emitCOFFLinkVTable(const MCSymbol *VTable, uint64_t Offset,
                          StringRef Class, StringRef Base) override;
  void emitCOFFLinkFact(uint64_t Kind) override;
  void emitCOFFKCFIThunk(const MCSymbol *Thunk, unsigned Kind, uint32_t Type,
                         uint32_t Marker, uint32_t Offset,
                         const MCSymbol *Mismatch) override;
  void emitCOFFLinkBinding(const MCSymbol *Symbol, unsigned Role) override;
  void emitCOFFLinkForwarder(const MCSymbol *Forwarder,
                             const MCSymbol *Pointer) override;
  void emitCOFFLinkIFunc(const MCSymbol *Function,
                         const MCSymbol *Resolver) override;
  void emitCOFFLinkIFuncFallback(const MCSymbol *Function,
                                 const MCSymbol *Fallback) override;
  void emitCOFFKCFIMemberTag(const MCSymbol *Prefix) override;
  void emitCOFFLinkContract(const MCSymbol *Owner, const MCSymbol *Symbol,
                            bool IsData, StringRef Identity,
                            ArrayRef<uint8_t> Digest) override;
  void emitCOFFLinkFusedCall(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkFusedRead() override;
  void emitCOFFLinkFusedAccess(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkFusedAdd(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkFusedIndex(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkValueOnly(const MCSymbol *Page) override;
  void emitCOFFLinkAnchor(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkTailJump() override;
  void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                        Align ByteAlignment) override;
  void emitLocalCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                             Align ByteAlignment) override;
  void emitWeakReference(MCSymbol *Alias, const MCSymbol *Symbol) override;
  void emitIdent(StringRef IdentString) override;
  void emitWinEHHandlerData(SMLoc Loc) override;
  void emitCGProfileEntry(const MCSymbolRefExpr *From,
                          const MCSymbolRefExpr *To, uint64_t Count) override;
  void finishImpl() override;

  /// \}

protected:
  MCSymbol *CurSymbol;

  /// The descriptions of the target's instructions, which a target streamer
  /// sets where the object's link-only records describe instructions, so that
  /// the facts it is given about instructions that read the register a load
  /// filled are checked against the instructions it emits.
  std::unique_ptr<const MCInstrInfo> MCII;

  void finalizeCGProfileEntry(const MCSymbolRefExpr *&S);

  /// Notes Inst, which the target streamer emitted where the current fragment
  /// F held Start bytes, and which copies CopySrc in full to the register it
  /// writes if CopySrc is set.
  void noteInstruction(const MCInst &Inst, const MCFragment *F, uint64_t Start,
                       MCRegister CopySrc = MCRegister());
  /// Reports Inst, which a fact says reads Reg, if it does not.
  void checkFusedUse(const MCInst &Inst, MCRegister Reg);

private:
  // What a register unit holds as far as the noted instructions show: the
  // value that the instruction that began at Start in Frag gave it, directly
  // or through copies, or a value of no load for a null Frag; while Run is the
  // current run, the instructions since the last that execution may not fall
  // through from, and otherwise a value from code the streamer cannot see.
  struct FusionValue {
    const MCFragment *Frag = nullptr;
    uint64_t Start = 0;
    unsigned Run = 0;
    bool Copied = false;
    bool operator==(const FusionValue &O) const {
      return Frag == O.Frag && Start == O.Start && Run == O.Run &&
             Copied == O.Copied;
    }
  };
  SmallVector<FusionValue, 0> FusionUnits;
  unsigned FusionRun = 1;
  // Whether a label has been emitted since the last noted instruction.
  bool FusionLabel = false;
  // Where each labeled load begins, and the register it fills.
  DenseMap<std::pair<const MCFragment *, uint64_t>, MCRegister> FusionLoads;
  // The loads that facts name, checked once every load is emitted: each must
  // fill Reg, if set.
  struct FusionCheck {
    const MCSymbol *Load;
    MCRegister Reg;
    SMLoc Loc;
  };
  SmallVector<FusionCheck, 0> FusionChecks;

  FusionValue getFusionValue(MCRegister Reg) const;
  void checkFusedLoad(const MCSymbol *Load, MCRegister Reg, bool Copies);

  void Error(const Twine &Msg) const;
};

} // end namespace llvm

#endif // LLVM_MC_MCWINCOFFSTREAMER_H
