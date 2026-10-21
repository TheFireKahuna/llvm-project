//===- llvm/MC/MCWinCOFFStreamer.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains an implementation of a Windows COFF object file streamer.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCCodeView.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCSymbolCOFF.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <cstdint>

using namespace llvm;

#define DEBUG_TYPE "WinCOFFStreamer"

/// MCExpr that represents the physical number for the sections that contains
/// a symbol.
class MCCOFFSectionNumberTargetExpr final : public MCTargetExpr {
  const MCSymbol &SectionSymbol;
  const WinCOFFObjectWriter &Writer;

  MCCOFFSectionNumberTargetExpr(const MCSymbol &SectionSymbol_,
                                const WinCOFFObjectWriter &Writer_)
      : SectionSymbol(SectionSymbol_), Writer(Writer_) {}

public:
  static MCCOFFSectionNumberTargetExpr *
  create(const MCSymbol &SectionSymbol, const WinCOFFObjectWriter &Writer,
         MCContext &Ctx) {
    return new (Ctx) MCCOFFSectionNumberTargetExpr(SectionSymbol, Writer);
  }

  void printImpl(raw_ostream &OS, const MCAsmInfo *MAI) const override {
    OS << ":secnum:";
    SectionSymbol.print(OS, MAI);
  }

  bool evaluateAsRelocatableImpl(MCValue &Res,
                                 const MCAssembler *Asm) const override {
    auto sectionNumber = Writer.getSectionNumber(SectionSymbol.getSection());
    assert(sectionNumber != 0 &&
           "Containing section was not assigned a number");
    Res = MCValue::get(sectionNumber);
    return true;
  }

  void visitUsedExpr(MCStreamer &Streamer) const override {
    // Contains no sub-expressions.
  }

  MCFragment *findAssociatedFragment() const override {
    return SectionSymbol.getFragment();
  }
};

/// MCExpr that represents the offset to a symbol from the beginning of its
/// section.
class MCCOFFSectionOffsetTargetExpr final : public MCTargetExpr {
  const MCSymbol &Symbol;

  MCCOFFSectionOffsetTargetExpr(const MCSymbol &Symbol_) : Symbol(Symbol_) {}

public:
  static MCCOFFSectionOffsetTargetExpr *create(const MCSymbol &Symbol,
                                               MCContext &Ctx) {
    return new (Ctx) MCCOFFSectionOffsetTargetExpr(Symbol);
  }

  void printImpl(raw_ostream &OS, const MCAsmInfo *MAI) const override {
    OS << ":secoffset:";
    Symbol.print(OS, MAI);
  }

  bool evaluateAsRelocatableImpl(MCValue &Res,
                                 const MCAssembler *Asm) const override {
    uint64_t CallsiteOffset = 0;
    if (!Asm->getSymbolOffset(Symbol, CallsiteOffset)) {
      return true;
    }
    Res = MCValue::get(CallsiteOffset);
    return true;
  }

  void visitUsedExpr(MCStreamer &Streamer) const override {
    // Contains no sub-expressions.
  }

  MCFragment *findAssociatedFragment() const override {
    return Symbol.getFragment();
  }
};

MCWinCOFFStreamer::MCWinCOFFStreamer(MCContext &Context,
                                     std::unique_ptr<MCAsmBackend> MAB,
                                     std::unique_ptr<MCCodeEmitter> CE,
                                     std::unique_ptr<MCObjectWriter> OW)
    : MCObjectStreamer(Context, std::move(MAB), std::move(OW), std::move(CE)),
      CurSymbol(nullptr) {
  if (Context.getTargetOptions().MCIncrementalLinkerCompatible)
    getWriter().setIncrementalLinkerCompatible(true);
}

WinCOFFObjectWriter &MCWinCOFFStreamer::getWriter() {
  return static_cast<WinCOFFObjectWriter &>(getAssembler().getWriter());
}

void MCWinCOFFStreamer::initSections(const MCSubtargetInfo &STI) {
  // FIXME: this is identical to the ELF one.
  // This emulates the same behavior of GNU as. This makes it easier
  // to compare the output as the major sections are in the same order.
  switchSection(getContext().getObjectFileInfo()->getTextSection());
  emitCodeAlignment(Align(4), STI);

  switchSection(getContext().getObjectFileInfo()->getDataSection());
  emitCodeAlignment(Align(4), STI);

  switchSection(getContext().getObjectFileInfo()->getBSSSection());
  emitCodeAlignment(Align(4), STI);

  switchSection(getContext().getObjectFileInfo()->getTextSection());
}

void MCWinCOFFStreamer::changeSection(MCSection *Section, uint32_t Subsection) {
  MCObjectStreamer::changeSection(Section, Subsection);
  ++FusionRun;
  // Ensure that the first and the second symbols relative to the section are
  // the section symbol and the COMDAT symbol.
  getAssembler().registerSymbol(*Section->getBeginSymbol());
  if (auto *Sym =
          static_cast<const MCSectionCOFF *>(Section)->getCOMDATSymbol())
    getAssembler().registerSymbol(*Sym);
}

void MCWinCOFFStreamer::emitLabel(MCSymbol *S, SMLoc Loc) {
  auto *Symbol = static_cast<MCSymbolCOFF *>(S);
  MCObjectStreamer::emitLabel(Symbol, Loc);
  FusionLabel = true;
}

bool MCWinCOFFStreamer::emitSymbolAttribute(MCSymbol *S,
                                            MCSymbolAttr Attribute) {
  auto *Symbol = static_cast<MCSymbolCOFF *>(S);
  getAssembler().registerSymbol(*Symbol);

  switch (Attribute) {
  default: return false;
  case MCSA_WeakReference:
  case MCSA_Weak:
    Symbol->setWeakExternalCharacteristics(COFF::IMAGE_WEAK_EXTERN_SEARCH_ALIAS);
    Symbol->setExternal(true);
    break;
  case MCSA_WeakAntiDep:
    Symbol->setWeakExternalCharacteristics(COFF::IMAGE_WEAK_EXTERN_ANTI_DEPENDENCY);
    Symbol->setExternal(true);
    Symbol->setIsWeakExternal(true);
    break;
  case MCSA_Global:
    Symbol->setExternal(true);
    break;
  case MCSA_AltEntry:
    llvm_unreachable("COFF doesn't support the .alt_entry attribute");
  }

  return true;
}

void MCWinCOFFStreamer::emitSymbolDesc(MCSymbol *Symbol, unsigned DescValue) {
  llvm_unreachable("not implemented");
}

void MCWinCOFFStreamer::beginCOFFSymbolDef(MCSymbol const *S) {
  if (CurSymbol)
    Error("starting a new symbol definition without completing the "
          "previous one");
  CurSymbol = static_cast<MCSymbolCOFF *>(const_cast<MCSymbol *>(S));
}

void MCWinCOFFStreamer::emitCOFFSymbolStorageClass(int StorageClass) {
  if (!CurSymbol) {
    Error("storage class specified outside of symbol definition");
    return;
  }

  if (StorageClass & ~COFF::SSC_Invalid) {
    Error("storage class value '" + Twine(StorageClass) +
               "' out of range");
    return;
  }

  getAssembler().registerSymbol(*CurSymbol);
  static_cast<MCSymbolCOFF *>(CurSymbol)->setClass((uint16_t)StorageClass);
}

void MCWinCOFFStreamer::emitCOFFSymbolType(int Type) {
  if (!CurSymbol) {
    Error("symbol type specified outside of a symbol definition");
    return;
  }

  if (Type & ~0xffff) {
    Error("type value '" + Twine(Type) + "' out of range");
    return;
  }

  getAssembler().registerSymbol(*CurSymbol);
  static_cast<const MCSymbolCOFF *>(CurSymbol)->setType((uint16_t)Type);
}

void MCWinCOFFStreamer::endCOFFSymbolDef() {
  if (!CurSymbol)
    Error("ending symbol definition without starting one");
  CurSymbol = nullptr;
}

void MCWinCOFFStreamer::emitCOFFSafeSEH(MCSymbol const *Symbol) {
  // SafeSEH is a feature specific to 32-bit x86.  It does not exist (and is
  // unnecessary) on all platforms which use table-based exception dispatch.
  if (getContext().getTargetTriple().getArch() != Triple::x86)
    return;

  auto *CSymbol = static_cast<const MCSymbolCOFF *>(Symbol);
  if (CSymbol->isSafeSEH())
    return;

  MCSection *SXData = getContext().getObjectFileInfo()->getSXDataSection();
  pushSection();
  switchSection(SXData);
  SXData->ensureMinAlignment(Align(4));

  newSpecialFragment<MCSymbolIdFragment>(Symbol);
  getAssembler().registerSymbol(*Symbol);
  CSymbol->setIsSafeSEH();

  // The Microsoft linker requires that the symbol type of a handler be
  // function. Go ahead and oblige it here.
  CSymbol->setType(COFF::IMAGE_SYM_DTYPE_FUNCTION
                   << COFF::SCT_COMPLEX_TYPE_SHIFT);
  popSection();
}

void MCWinCOFFStreamer::emitCOFFSymbolIndex(MCSymbol const *Symbol) {
  MCSection *Sec = getCurrentSectionOnly();
  Sec->ensureMinAlignment(Align(4));

  newSpecialFragment<MCSymbolIdFragment>(Symbol);
  getAssembler().registerSymbol(*Symbol);
}

void MCWinCOFFStreamer::emitCOFFSectionIndex(const MCSymbol *Symbol) {
  visitUsedSymbol(*Symbol);
  const MCSymbolRefExpr *SRE = MCSymbolRefExpr::create(Symbol, getContext());
  ensureHeadroom(2);
  addFixup(SRE, FK_SecRel_2);
  appendContents(2, 0);
}

void MCWinCOFFStreamer::emitCOFFSecRel32(const MCSymbol *Symbol,
                                         uint64_t Offset) {
  visitUsedSymbol(*Symbol);
  // Create Symbol A for the relocation relative reference.
  const MCExpr *MCE = MCSymbolRefExpr::create(Symbol, getContext());
  // Add the constant offset, if given.
  if (Offset)
    MCE = MCBinaryExpr::createAdd(
        MCE, MCConstantExpr::create(Offset, getContext()), getContext());
  ensureHeadroom(4);
  addFixup(MCE, FK_SecRel_4);
  // Emit 4 bytes (zeros) to the object file.
  appendContents(4, 0);
}

void MCWinCOFFStreamer::emitCOFFImgRel32(const MCSymbol *Symbol,
                                         int64_t Offset) {
  visitUsedSymbol(*Symbol);
  // Create Symbol A for the relocation relative reference.
  const MCExpr *MCE = MCSymbolRefExpr::create(
      Symbol, MCSymbolRefExpr::VK_COFF_IMGREL32, getContext());
  // Add the constant offset, if given.
  if (Offset)
    MCE = MCBinaryExpr::createAdd(
        MCE, MCConstantExpr::create(Offset, getContext()), getContext());
  ensureHeadroom(4);
  addFixup(MCE, FK_Data_4);
  // Emit 4 bytes (zeros) to the object file.
  appendContents(4, 0);
}

void MCWinCOFFStreamer::emitCOFFSecNumber(MCSymbol const *Symbol) {
  visitUsedSymbol(*Symbol);
  // Create Symbol for section number.
  const MCExpr *MCE = MCCOFFSectionNumberTargetExpr::create(
      *Symbol, this->getWriter(), getContext());
  ensureHeadroom(4);
  addFixup(MCE, FK_Data_4);
  // Emit 4 bytes (zeros) to the object file.
  appendContents(4, 0);
}

void MCWinCOFFStreamer::emitCOFFSecOffset(MCSymbol const *Symbol) {
  visitUsedSymbol(*Symbol);
  // Create Symbol for section offset.
  const MCExpr *MCE =
      MCCOFFSectionOffsetTargetExpr::create(*Symbol, getContext());
  ensureHeadroom(4);
  addFixup(MCE, FK_Data_4);
  // Emit 4 bytes (zeros) to the object file.
  appendContents(4, 0);
}

void MCWinCOFFStreamer::emitCOFFLinkPin(MCSymbol const *Symbol,
                                        unsigned Log2Modulus, uint64_t Residue,
                                        bool Required) {
  getWriter().addLinkPin({Symbol, Log2Modulus, Residue, Required});
}

void MCWinCOFFStreamer::emitCOFFLinkVTable(const MCSymbol *VTable,
                                           uint64_t Offset, StringRef Class,
                                           StringRef Base) {
  getWriter().addLinkVTable({VTable, Offset, Class.str(), Base.str()});
}

void MCWinCOFFStreamer::emitCOFFLinkFact(uint64_t Kind) {
  getWriter().addLinkFact(Kind);
}

void MCWinCOFFStreamer::emitCOFFKCFIThunk(const MCSymbol *Thunk, unsigned Kind,
                                          uint32_t Type, uint32_t Marker,
                                          uint32_t Offset,
                                          const MCSymbol *Mismatch) {
  getWriter().addKCFIThunk({Thunk, Kind, Type, Marker, Offset, Mismatch});
}

void MCWinCOFFStreamer::emitCOFFLinkBinding(const MCSymbol *Symbol,
                                            unsigned Role) {
  getWriter().addLinkBinding({Symbol, Role});
}

void MCWinCOFFStreamer::emitCOFFLinkForwarder(const MCSymbol *Forwarder,
                                              const MCSymbol *Pointer) {
  getWriter().addLinkSymbolPair(
      {COFF::LinkRecordForwarders, Forwarder, Pointer});
}

void MCWinCOFFStreamer::emitCOFFLinkIFunc(const MCSymbol *Function,
                                          const MCSymbol *Resolver) {
  // The record must reach the object even when nothing else names the
  // resolver, or the stub would jump through a pointer to itself.
  getAssembler().registerSymbol(*Function);
  getAssembler().registerSymbol(*Resolver);
  getWriter().addLinkSymbolPair({COFF::LinkRecordIFuncs, Function, Resolver});
}

void MCWinCOFFStreamer::emitCOFFLinkIFuncFallback(const MCSymbol *Function,
                                                  const MCSymbol *Fallback) {
  getAssembler().registerSymbol(*Function);
  getAssembler().registerSymbol(*Fallback);
  getWriter().addLinkSymbolPair(
      {COFF::LinkRecordIFuncFallbacks, Function, Fallback});
}

void MCWinCOFFStreamer::emitCOFFKCFIMemberTag(const MCSymbol *Prefix) {
  getWriter().addKCFIMemberTag(Prefix);
}

void MCWinCOFFStreamer::emitCOFFLinkContract(const MCSymbol *Owner,
                                             const MCSymbol *Symbol,
                                             bool IsData, StringRef Identity,
                                             ArrayRef<uint8_t> Digest) {
  getWriter().addLinkContract(Owner, Symbol, IsData, Identity, Digest);
}

// The value Reg holds, or a value of no load where its units hold different
// ones. An artificial unit, such as one a target gives the upper half of a
// register that writing its lower half clears, holds no value of its own.
MCWinCOFFStreamer::FusionValue
MCWinCOFFStreamer::getFusionValue(MCRegister Reg) const {
  const MCRegisterInfo &MRI = *getContext().getRegisterInfo();
  std::optional<FusionValue> Value;
  for (MCRegUnit U : MRI.regunits(Reg)) {
    if (MRI.isArtificialRegUnit(U))
      continue;
    FusionValue UnitValue;
    if (!FusionUnits.empty() &&
        FusionUnits[static_cast<unsigned>(U)].Run == FusionRun)
      UnitValue = FusionUnits[static_cast<unsigned>(U)];
    if (Value && !(*Value == UnitValue))
      return {nullptr, 0, FusionRun, false};
    Value = UnitValue;
  }
  return Value.value_or(FusionValue());
}

// A fact names the load whose register an instruction reads, which a linker
// may then rewrite as though nothing else wrote it. In the current run the
// value in Reg must be the load's, or with Copies a copy of it, where the
// load's own value must also be in the register it fills; that the load is a
// load is checked once every load is emitted. Where the run began after the
// load, the instructions that may have written Reg are out of view.
void MCWinCOFFStreamer::checkFusedLoad(const MCSymbol *Load, MCRegister Reg,
                                       bool Copies) {
  SMLoc Loc = getStartTokLoc();
  FusionValue Value = getFusionValue(Reg);
  if (Value.Run == FusionRun &&
      (Value.Frag ? Value.Frag != Load->getFragment() ||
                        Value.Start != Load->getOffset()
                  : !Value.Copied)) {
    getContext().reportError(
        Loc, "fused instruction reads " +
                 Twine(getContext().getRegisterInfo()->getName(Reg)) +
                 ", which an instruction other than the load at '" +
                 Load->getName() + "' last wrote");
    return;
  }
  if (Value.Run == FusionRun && Value.Copied && !Copies) {
    getContext().reportError(
        Loc, "fused instruction reads " +
                 Twine(getContext().getRegisterInfo()->getName(Reg)) +
                 ", which a copy last wrote");
    return;
  }
  FusionChecks.push_back({Load, Copies ? MCRegister() : Reg, Loc});
}

void MCWinCOFFStreamer::noteInstruction(const MCInst &Inst,
                                        const MCFragment *F, uint64_t Start,
                                        MCRegister CopySrc) {
  const MCInstrDesc &Desc = MCII->get(Inst.getOpcode());
  const MCRegisterInfo &MRI = *getContext().getRegisterInfo();
  // A fact names a load by a label where it begins.
  if (FusionLabel && Desc.mayLoad() && Desc.getNumDefs() &&
      Inst.getOperand(0).isReg())
    FusionLoads[{F, Start}] = Inst.getOperand(0).getReg();
  FusionLabel = false;
  // A call may not return, and execution does not fall through a jump, a
  // return or a trap, so that what follows is reached from elsewhere.
  if (Desc.isCall() || Desc.isBarrier() || Desc.isTrap()) {
    ++FusionRun;
    return;
  }

  if (FusionUnits.empty())
    FusionUnits.resize(MRI.getNumRegUnits());
  FusionValue Written{F, Start, FusionRun, false};
  // A copy carries the value it copies within the run: a load's, or one from
  // before the run.
  FusionValue Copy = Written;
  if (CopySrc) {
    FusionValue Src = getFusionValue(CopySrc);
    if (Src.Run != FusionRun)
      Copy = {nullptr, 0, FusionRun, true};
    else if (Src.Frag)
      Copy = {Src.Frag, Src.Start, FusionRun, true};
  }
  auto Write = [&](MCRegister Reg, const FusionValue &Value) {
    for (MCRegUnit U : MRI.regunits(Reg))
      if (!MRI.isArtificialRegUnit(U))
        FusionUnits[static_cast<unsigned>(U)] = Value;
  };
  for (unsigned I = 0, E = Inst.getNumOperands(); I != E; ++I) {
    const MCOperand &MO = Inst.getOperand(I);
    if ((I < Desc.getNumDefs() ||
         (Desc.variadicOpsAreDefs() && I >= Desc.getNumOperands())) &&
        MO.isReg() && MO.getReg())
      Write(MO.getReg(), CopySrc && I == 0 ? Copy : Written);
  }
  for (MCPhysReg Reg : Desc.implicit_defs())
    Write(Reg, Written);
}

void MCWinCOFFStreamer::checkFusedUse(const MCInst &Inst, MCRegister Reg) {
  const MCInstrDesc &Desc = MCII->get(Inst.getOpcode());
  const MCRegisterInfo &MRI = *getContext().getRegisterInfo();
  for (unsigned I = Desc.getNumDefs(), E = Inst.getNumOperands(); I != E; ++I)
    if (Inst.getOperand(I).isReg() && Inst.getOperand(I).getReg() &&
        MRI.regsOverlap(Inst.getOperand(I).getReg(), Reg))
      return;
  if (any_of(Desc.implicit_uses(),
             [&](MCPhysReg Use) { return MRI.regsOverlap(Use, Reg); }))
    return;
  getContext().reportError(getStartTokLoc(),
                           "fused instruction does not read " +
                               Twine(MRI.getName(Reg)));
}

// A fused call is known only once a target's streamer has seen it in the form
// the records describe. A fused read names the place where the load begins,
// which the object's records match to the load's site.
void MCWinCOFFStreamer::emitCOFFLinkFusedCall(const MCSymbol *Load,
                                              MCRegister Reg) {
  if (MCII)
    checkFusedLoad(Load, Reg, /*Copies=*/false);
  getWriter().addLinkFusedCall(
      {nullptr, Load, getContext().getRegisterInfo()->getEncodingValue(Reg),
       false});
}

void MCWinCOFFStreamer::emitCOFFLinkFusedRead() {
  MCSymbol *Site = getContext().createTempSymbol();
  emitLabel(Site);
  getWriter().addLinkFusedRead({Site, nullptr, false});
}

// A fused access or add is known only once a target's streamer has seen it in
// a form the records describe.
void MCWinCOFFStreamer::emitCOFFLinkFusedAccess(const MCSymbol *Load,
                                                MCRegister Reg) {
  if (MCII)
    checkFusedLoad(Load, Reg, /*Copies=*/true);
  getWriter().addLinkFusedAccess({nullptr, Load, 0, 0, 0, 0, false, 0, 0});
}

void MCWinCOFFStreamer::emitCOFFLinkFusedIndex(const MCSymbol *Load,
                                               MCRegister Reg) {
  if (MCII)
    checkFusedLoad(Load, Reg, /*Copies=*/true);
  getWriter().addLinkFusedAccess(
      {nullptr, Load, 0, 0, 0, 0, false, 0, 0, /*Index=*/true});
}

void MCWinCOFFStreamer::emitCOFFLinkFusedAdd(const MCSymbol *Load,
                                             MCRegister Reg) {
  if (MCII)
    checkFusedLoad(Load, Reg, /*Copies=*/false);
  getWriter().addLinkFusedAdd({nullptr, Load, 0, 0});
}

// A jump that ends its function names the place where it begins.
void MCWinCOFFStreamer::emitCOFFLinkTailJump() {
  MCSymbol *Site = getContext().createTempSymbol();
  emitLabel(Site);
  getWriter().addLinkTailJump(Site);
}

// A value-only load names the place where the load begins, as a fused read
// does.
void MCWinCOFFStreamer::emitCOFFLinkValueOnly(const MCSymbol *Page) {
  MCSymbol *Site = getContext().createTempSymbol();
  emitLabel(Site);
  getWriter().addLinkValueOnly({Site, Page});
}

// So does a load that may derive its pointer from another's register.
void MCWinCOFFStreamer::emitCOFFLinkAnchor(const MCSymbol *Load,
                                           MCRegister Reg) {
  if (MCII)
    checkFusedLoad(Load, Reg, /*Copies=*/false);
  MCSymbol *Site = getContext().createTempSymbol();
  emitLabel(Site);
  getWriter().addLinkAnchor(
      {Site, Load, getContext().getRegisterInfo()->getEncodingValue(Reg)});
}

void MCWinCOFFStreamer::emitCommonSymbol(MCSymbol *S, uint64_t Size,
                                         Align ByteAlignment) {
  auto *Symbol = static_cast<MCSymbolCOFF *>(S);

  const Triple &T = getContext().getTargetTriple();
  if (T.isWindowsMSVCEnvironment()) {
    if (ByteAlignment > 32)
      report_fatal_error("alignment is limited to 32-bytes");

    // Round size up to alignment so that we will honor the alignment request.
    Size = std::max(Size, ByteAlignment.value());
  }

  getAssembler().registerSymbol(*Symbol);
  Symbol->setExternal(true);
  Symbol->setCommon(Size, ByteAlignment);

  if (!T.isWindowsMSVCEnvironment() && ByteAlignment > 1) {
    SmallString<128> Directive;
    raw_svector_ostream OS(Directive);
    const MCObjectFileInfo *MFI = getContext().getObjectFileInfo();

    OS << " -aligncomm:\"" << Symbol->getName() << "\","
       << Log2_32_Ceil(ByteAlignment.value());

    pushSection();
    switchSection(MFI->getDrectveSection());
    emitBytes(Directive);
    popSection();
  }
}

void MCWinCOFFStreamer::emitLocalCommonSymbol(MCSymbol *S, uint64_t Size,
                                              Align ByteAlignment) {
  auto *Symbol = static_cast<MCSymbolCOFF *>(S);

  MCSection *Section = getContext().getObjectFileInfo()->getBSSSection();
  pushSection();
  switchSection(Section);
  emitValueToAlignment(ByteAlignment, 0, 1, 0);
  emitLabel(Symbol);
  Symbol->setExternal(false);
  emitZeros(Size);
  popSection();
}

// Hack: Used by llvm-ml to implement the alias directive.
void MCWinCOFFStreamer::emitWeakReference(MCSymbol *AliasS,
                                          const MCSymbol *Symbol) {
  auto *Alias = static_cast<MCSymbolCOFF *>(AliasS);
  emitSymbolAttribute(Alias, MCSA_Weak);
  Alias->setIsWeakExternal(true);

  getAssembler().registerSymbol(*Symbol);
  Alias->setVariableValue(MCSymbolRefExpr::create(Symbol, getContext()));
}

// TODO: Implement this if you want to emit .comment section in COFF obj files.
void MCWinCOFFStreamer::emitIdent(StringRef IdentString) {
  llvm_unreachable("not implemented");
}

void MCWinCOFFStreamer::emitWinEHHandlerData(SMLoc Loc) {
  llvm_unreachable("not implemented");
}

void MCWinCOFFStreamer::emitCGProfileEntry(const MCSymbolRefExpr *From,
                                           const MCSymbolRefExpr *To,
                                           uint64_t Count) {
  // Ignore temporary symbols for now.
  if (!From->getSymbol().isTemporary() && !To->getSymbol().isTemporary())
    getWriter().getCGProfile().push_back({From, To, Count});
}

void MCWinCOFFStreamer::finalizeCGProfileEntry(const MCSymbolRefExpr *&SRE) {
  const MCSymbol *S = &SRE->getSymbol();
  if (getAssembler().registerSymbol(*S))
    static_cast<const MCSymbolCOFF *>(S)->setExternal(true);
}

void MCWinCOFFStreamer::finishImpl() {
  // Every load that a fact names is emitted by now. One that is not defined
  // the object writer reports.
  const MCRegisterInfo &MRI = *getContext().getRegisterInfo();
  for (const FusionCheck &Check : FusionChecks) {
    if (!Check.Load->isInSection())
      continue;
    auto Load = FusionLoads.find(
        {Check.Load->getFragment(), Check.Load->getOffset()});
    if (Load == FusionLoads.end())
      getContext().reportError(Check.Loc,
                               "fused instruction reads the instruction at '" +
                                   Check.Load->getName() +
                                   "', which is not a load");
    else if (Check.Reg && !MRI.isSubRegisterEq(Check.Reg, Load->second))
      getContext().reportError(Check.Loc, "fused instruction reads " +
                                              Twine(MRI.getName(Check.Reg)) +
                                              ", which the load at '" +
                                              Check.Load->getName() +
                                              "' does not fill");
  }

  getContext().getCVContext().finish();
  MCAssembler &Asm = getAssembler();
  if (Asm.getWriter().getEmitAddrsigSection()) {
    // Register the section.
    switchSection(Asm.getContext().getCOFFSection(".llvm_addrsig",
                                                  COFF::IMAGE_SCN_LNK_REMOVE));
  }
  if (!Asm.getWriter().getCGProfile().empty()) {
    for (auto &E : Asm.getWriter().getCGProfile()) {
      finalizeCGProfileEntry(E.From);
      finalizeCGProfileEntry(E.To);
    }
    switchSection(Asm.getContext().getCOFFSection(".llvm.call-graph-profile",
                                                  COFF::IMAGE_SCN_LNK_REMOVE));
  }
  if (getWriter().hasLinkRecordsSection())
    switchSection(Asm.getContext().getCOFFSection(".llvm_link_records",
                                                  COFF::IMAGE_SCN_LNK_REMOVE));

  MCObjectStreamer::finishImpl();
}

void MCWinCOFFStreamer::Error(const Twine &Msg) const {
  getContext().reportError(SMLoc(), Msg);
}
