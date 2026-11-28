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
#include "llvm/MC/MCInst.h"
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
  case MCSA_WeakDefinition:
    // GNU as's form of a weak definition.
    Symbol->setWeakExternalCharacteristics(
        COFF::IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY);
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

void MCWinCOFFStreamer::emitELFSize(MCSymbol *Symbol, const MCExpr *Value) {
  if (getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment())
    static_cast<MCSymbolCOFF *>(Symbol)->setSize(Value);
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

void MCWinCOFFStreamer::emitCOFFLinkFact(const MCSymbol *Symbol, uint64_t Tag,
                                         ArrayRef<const MCExpr *> Operands) {
  // A fact names its symbols by their index in the symbol table.
  getAssembler().registerSymbol(*Symbol);
  for (const MCExpr *Operand : Operands)
    visitUsedExpr(*Operand);
  getWriter().LinkFacts.push_back(
      {Symbol, Tag, {Operands.begin(), Operands.end()}});
}

void MCWinCOFFStreamer::emitCOFFSwitchTable(
    const MCSymbol *Branch, const MCSymbol *Table, const MCSymbol *Base,
    uint64_t Count, uint64_t Type, const MCSymbol *Load, uint64_t Flags) {
  getWriter().SwitchTables.push_back(
      {{Branch, Table, Base, Load}, Count, Type, Flags});
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

static uint16_t getMachine(const MCContext &Context) {
  return Context.getTargetTriple().isX86_64() ? COFF::IMAGE_FILE_MACHINE_AMD64
                                              : COFF::IMAGE_FILE_MACHINE_ARM64;
}

// What a fused site is called, by how it reads its register.
static const char *const FusedKinds[] = {"",    "",       "call", "jump",
                                         "add", "access", "test"};

void MCWinCOFFStreamer::reportMisplacedFusedSite() {
  getContext().reportError(FusedSite.Loc,
                           Twine("fused ") + FusedKinds[size_t(FusedSite.Use)] +
                               (COFF::relocationNamesNoSymbol(FusedSite.Type)
                                    ? " is not at a "
                                    : " does not name a pointer at a ") +
                               FusedForms[size_t(FusedSite.Use)]);
}

void MCWinCOFFStreamer::endResizableCode(MCFragment *From) {
  MCFragment *Last = getCurrentFragment();
  if (!From)
    From = Last;
  MCSection *Sec = From->getParent();
  if (!Sec->isLinkerRelaxable())
    Sec->setFirstLinkerRelaxable(From->getLayoutOrder());
  for (MCFragment *F = From; F != Last; F = F->getNext())
    F->setLinkerRelaxable();
  Last->setLinkerRelaxable();
  newFragment();
}

void MCWinCOFFStreamer::finishInstruction(bool Resizable, MCFragment *From) {
  if (SiteInsts && !--SiteInsts)
    Resizable = true;
  if (Resizable)
    endResizableCode(From);
}

void MCWinCOFFStreamer::emitRelocDirective(const MCExpr &Offset, StringRef Name,
                                           const MCExpr *Expr, SMLoc Loc) {
  std::optional<MCFixupKind> Kind =
      getAssembler().getBackend().getFixupKind(Name);
  uint16_t Machine = getMachine(getContext());
  uint16_t Type =
      Kind && mc::isRelocRelocation(*Kind) ? getCOFFRelocType(*Kind) : 0;
  COFF::FusedUse Use =
      MCII ? COFF::getFusedMarkerUse(Machine, Type) : COFF::FusedUse::None;
  // The type of a fused jump is that of its site, which records it, as it
  // does a marker whose index it gives.
  if (Use != COFF::FusedUse::Jump &&
      (Use == COFF::FusedUse::None || !COFF::relocationNamesNoSymbol(Type)))
    MCObjectStreamer::emitRelocDirective(Offset, Name, Expr, Loc);
  if (COFF::relocationMovesCode(Type))
    MovingSections.insert(getCurrentSectionOnly());
  // A marker of code that the linker may resize is at its site: padding ends
  // where its marker is, and any other site with the instructions after the
  // marker. A target finds the end of a fused site, which the check of its
  // form places.
  if (MCII && Use == COFF::FusedUse::None &&
      COFF::relocationMayResizeCode(Machine, Type)) {
    MCValue Site;
    if (!Offset.evaluateAsRelocatable(Site, nullptr) || !Site.getAddSym() ||
        Site.getSubSym() ||
        Site.getAddSym()->getFragment() != getCurrentFragment() ||
        Site.getAddSym()->getOffset() + Site.getConstant() != getCurFragSize())
      getContext().reportError(Loc, Name + " marks code that the linker may "
                                           "resize and is not at that code");
    else if (Type == COFF::IMAGE_REL_LLVM_ALIGN)
      endResizableCode();
    else
      SiteInsts = Type == COFF::IMAGE_REL_LLVM_TLS_SCALE ? TlsScaleInsts : 1;
  }
  if (Use == COFF::FusedUse::None)
    return;
  if (FusedSite.Offset)
    reportMisplacedFusedSite();
  auto *Pointer = dyn_cast_or_null<MCSymbolRefExpr>(Expr);
  FusedSite = {
      &Offset, Pointer ? &Pointer->getSymbol() : nullptr, Loc, Type, Use, Expr};
}

bool MCWinCOFFStreamer::readsFusedRegisterOnce(const MCInst &Inst,
                                               const MCInstrDesc &Desc,
                                               const MCRegisterInfo &MRI,
                                               COFF::FusedUse Use,
                                               unsigned RegOp) {
  MCRegister Reg = Inst.getOperand(RegOp).getReg();
  if (any_of(Desc.implicit_uses(), [&](MCPhysReg Implicit) {
        return MRI.regsOverlap(Implicit, Reg);
      }))
    return false;
  for (unsigned I = Desc.getNumDefs(), E = Inst.getNumOperands(); I != E; ++I)
    if (I != RegOp && (Use != COFF::FusedUse::Test || I != RegOp + 1) &&
        Inst.getOperand(I).isReg() && Inst.getOperand(I).getReg() &&
        MRI.regsOverlap(Inst.getOperand(I).getReg(), Reg))
      return false;
  return true;
}

// The linker rewrites a fused site as its type says, so each is checked
// against the instructions the run shows; what wrote a register before the
// run began rests on whoever states the type.
MCSymbol *MCWinCOFFStreamer::checkFused(const MCInst &Inst, COFF::FusedUse Use,
                                        unsigned RegOp, const MCSymbol *Load,
                                        bool IsRead, bool Tail) {
  const MCRegisterInfo &MRI = *getContext().getRegisterInfo();
  MCSymbol *Marker = nullptr;
  if (RunSection != getCurrentSection())
    RunWrites.clear();
  RunSection = getCurrentSection();
  const MCInstrDesc &Desc = MCII->get(Inst.getOpcode());
  // A fused site begins at its marker, reads its register as the marker's
  // type says, in one operand only, but a test against itself, and reads
  // there what a load of the marker's pointer last wrote, which only an
  // access may read through a copy.
  if (FusedSite.Offset) {
    MCRegister Reg = Use != COFF::FusedUse::None
                         ? Inst.getOperand(RegOp).getReg()
                         : MCRegister();
    bool IsSite = Use == FusedSite.Use &&
                  readsFusedRegisterOnce(Inst, Desc, MRI, Use, RegOp);
    const char *Kind = FusedKinds[size_t(FusedSite.Use)];
    MCValue Site;
    bool Here =
        (FusedSite.Pointer || COFF::relocationNamesNoSymbol(FusedSite.Type)) &&
        FusedSite.Offset->evaluateAsRelocatable(Site, nullptr) &&
        Site.getAddSym() && !Site.getSubSym() &&
        Site.getAddSym()->getFragment() == getCurrentFragment() &&
        Site.getAddSym()->getOffset() + Site.getConstant() == getCurFragSize();
    if (Here && !Site.getConstant())
      Marker = const_cast<MCSymbol *>(Site.getAddSym());
    if (FusedSite.Use == COFF::FusedUse::Jump)
      MCObjectStreamer::emitRelocDirective(
          *FusedSite.Offset,
          IsSite && Tail ? "IMAGE_REL_LLVM_FUSED_JUMP_TAIL"
                         : "IMAGE_REL_LLVM_FUSED_JUMP",
          FusedSite.Expr, FusedSite.Loc);
    if (!Here || !IsSite)
      reportMisplacedFusedSite();
    else if (FusedSite.Pointer &&
             any_of(MRI.regunits(Reg), [&](MCRegUnit Unit) {
               auto Write = RunWrites.find(static_cast<unsigned>(Unit));
               if (Write == RunWrites.end())
                 return false;
               const MCSymbol *Pointer = Write->second.getPointer();
               return Write->second.getInt()
                          ? Use != COFF::FusedUse::Access ||
                                (Pointer && Pointer != FusedSite.Pointer)
                          : Pointer != FusedSite.Pointer;
             }))
      getContext().reportError(
          FusedSite.Loc, Twine("fused ") + Kind + " reads " + MRI.getName(Reg) +
                             ", which no load of '" +
                             FusedSite.Pointer->getName() + "' last wrote");
    // The linker may resize a site of one instruction with it.
    if (Here && IsSite && Use != COFF::FusedUse::Call &&
        Use != COFF::FusedUse::Jump &&
        COFF::relocationMayResizeCode(getMachine(getContext()), FusedSite.Type))
      SiteInsts = 1;
    FusedSite.Offset = nullptr;
    getWriter().LinkRecordsFlags |= COFF::LinkRecordsFusedChecked;
  }
  if (IsRead)
    getWriter().LinkRecordsFlags |= COFF::LinkRecordsFusedChecked;
  // A copy holds what every unit of its source does.
  PointerIntPair<const MCSymbol *, 1, bool> Held(Load, false);
  if (Use == COFF::FusedUse::Copy) {
    Held.setInt(true);
    bool First = true;
    for (MCRegUnit Unit : MRI.regunits(Inst.getOperand(RegOp).getReg())) {
      auto Write = RunWrites.find(static_cast<unsigned>(Unit));
      const MCSymbol *Pointer =
          Write == RunWrites.end() ? nullptr : Write->second.getPointer();
      if ((Write != RunWrites.end() && !Pointer) ||
          (!First && Pointer != Held.getPointer()))
        Held = {nullptr, false};
      else if (First)
        Held.setPointer(Pointer);
      First = false;
    }
  }
  auto Write = [&](MCRegister Reg,
                   PointerIntPair<const MCSymbol *, 1, bool> Held) {
    for (MCRegUnit Unit : MRI.regunits(Reg))
      RunWrites[static_cast<unsigned>(Unit)] = Held;
  };
  if (Desc.isCall() || Desc.isBarrier() || Desc.isTrap())
    RunWrites.clear();
  else {
    for (unsigned I = 0, E = Desc.getNumDefs(); I != E; ++I)
      if (Inst.getOperand(I).isReg() && Inst.getOperand(I).getReg())
        Write(Inst.getOperand(I).getReg(), Held);
    for (MCPhysReg Reg : Desc.implicit_defs())
      Write(Reg, {});
  }
  return Marker;
}

void MCWinCOFFStreamer::finishImpl() {
  if (FusedSite.Offset)
    reportMisplacedFusedSite();
  getContext().getCVContext().finish();
  MCAssembler &Asm = getAssembler();
  if (Asm.getWriter().getEmitAddrsigSection()) {
    // Register the section.
    switchSection(Asm.getContext().getCOFFSection(".llvm_addrsig",
                                                  COFF::IMAGE_SCN_LNK_REMOVE));
  }
  if (getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
    // An object for these environments carries link records, and names the
    // symbol that only a linker which reads them defines.
    switchSection(getContext().getObjectFileInfo()->getDrectveSection());
    emitBytes(" /INCLUDE:__llvm_link_records_v1");
    // NT-POSIX's objects state the owned thread-local model, which an image
    // has one revision of.
    if (getContext().getTargetTriple().isWindowsNTPOSIXEnvironment())
      emitBytes(" /FAILIFMISMATCH:_TLS_MODEL=ntposix.1");
    switchSection(Asm.getContext().getCOFFSection(".llvm_link_records",
                                                  COFF::IMAGE_SCN_LNK_INFO));
    // Register the tables of address-taken functions and import pointers,
    // which the writer fills from the object's relocations.
    const MCObjectFileInfo *OFI = getContext().getObjectFileInfo();
    for (MCSection *Table : {OFI->getGFIDsSection(), OFI->getGIATsSection()}) {
      switchSection(Table);
      Table->ensureMinAlignment(Align(4));
    }
  }
  if (!Asm.getWriter().getCGProfile().empty()) {
    for (auto &E : Asm.getWriter().getCGProfile()) {
      finalizeCGProfileEntry(E.From);
      finalizeCGProfileEntry(E.To);
    }
    switchSection(Asm.getContext().getCOFFSection(".llvm.call-graph-profile",
                                                  COFF::IMAGE_SCN_LNK_REMOVE));
  }

  MCObjectStreamer::finishImpl();
}

void MCWinCOFFStreamer::Error(const Twine &Msg) const {
  getContext().reportError(SMLoc(), Msg);
}
