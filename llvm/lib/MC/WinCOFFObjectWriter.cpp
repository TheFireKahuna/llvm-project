//===- llvm/MC/WinCOFFObjectWriter.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains an implementation of a Win32 COFF object file writer.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCSymbolCOFF.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Support/CRC.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
using llvm::support::endian::write32le;

#define DEBUG_TYPE "WinCOFFObjectWriter"

namespace {

constexpr int OffsetLabelIntervalBits = 20;

using name = SmallString<COFF::NameSize>;

enum AuxiliaryType {
  ATWeakExternal,
  ATFile,
  ATSectionDefinition,
  ATFunctionDefinition
};

struct AuxSymbol {
  AuxiliaryType AuxType;
  COFF::Auxiliary Aux;
};

class COFFSection;

class COFFSymbol {
public:
  COFF::symbol Data = {};

  using AuxiliarySymbols = SmallVector<AuxSymbol, 1>;

  name Name;
  int Index = 0;
  AuxiliarySymbols Aux;
  COFFSymbol *Other = nullptr;
  COFFSection *Section = nullptr;
  int Relocations = 0;
  const MCSymbol *MC = nullptr;

  COFFSymbol(StringRef Name) : Name(Name) {}

  void set_name_offset(uint32_t Offset);

  int64_t getIndex() const { return Index; }
  void setIndex(int Value) {
    Index = Value;
    if (MC)
      MC->setIndex(static_cast<uint32_t>(Value));
  }
};

// This class contains staging data for a COFF relocation entry.
struct COFFRelocation {
  COFF::relocation Data;
  COFFSymbol *Symb = nullptr;

  COFFRelocation() = default;

  static size_t size() { return COFF::RelocationSize; }
};

using relocations = std::vector<COFFRelocation>;

class COFFSection {
public:
  COFF::section Header = {};

  std::string Name;
  int Number = 0;
  MCSectionCOFF const *MCSection = nullptr;
  COFFSymbol *Symbol = nullptr;
  relocations Relocations;

  COFFSection(StringRef Name) : Name(std::string(Name)) {}

  SmallVector<COFFSymbol *, 1> OffsetSymbols;
};
} // namespace

class llvm::WinCOFFWriter {
  WinCOFFObjectWriter &OWriter;
  support::endian::Writer W;
  MCAssembler *Asm = nullptr;

  using symbols = std::vector<std::unique_ptr<COFFSymbol>>;
  using sections = std::vector<std::unique_ptr<COFFSection>>;

  using symbol_map = DenseMap<MCSymbol const *, COFFSymbol *>;
  using section_map = DenseMap<MCSection const *, COFFSection *>;
  using section_offset_map =
      DenseMap<std::pair<MCSection const *, uint64_t>, COFFSymbol *>;

  using symbol_list = DenseSet<COFFSymbol *>;

  // Root level file contents.
  COFF::header Header = {};
  sections Sections;
  symbols Symbols;
  StringTableBuilder Strings{StringTableBuilder::WinCOFF};

  // Maps used during object file creation.
  section_map SectionMap;
  symbol_map SymbolMap;
  section_offset_map SecRelSymbolMap;

  symbol_list WeakDefaults;

  bool UseBigObj;
  bool UseOffsetLabels = false;
  unsigned SecRelSymbolCount = 0;

public:
  enum DwoMode {
    AllSections,
    NonDwoOnly,
    DwoOnly,
  } Mode;

  WinCOFFWriter(WinCOFFObjectWriter &OWriter, raw_pwrite_stream &OS,
                DwoMode Mode);

  void reset();
  void setAssembler(MCAssembler *A) { Asm = A; }
  void executePostLayoutBinding();
  void recordRelocation(const MCFragment &F, const MCFixup &Fixup,
                        MCValue Target, uint64_t &FixedValue);
  uint64_t writeObject();
  int getSectionNumber(const MCSection &Section) const;

private:
  MCContext &getContext() const { return OWriter.getContext(); }
  COFFSymbol *createSymbol(StringRef Name);
  COFFSymbol *getOrCreateSecRelSymbol(const MCSymbol &Sym, uint64_t Offset);
  void recordSpan(const MCFragment &F, const MCFixup &Fixup, const MCSymbol &A,
                  const MCSymbol &B, int64_t Constant, uint64_t &FixedValue);
  COFFSymbol *getOrCreateCOFFSymbol(const MCSymbol &Sym);
  COFFSection *createSection(StringRef Name);

  void defineSection(MCSectionCOFF const &Sec);

  COFFSymbol *getLinkedSymbol(const MCSymbol &Symbol);
  void defineSymbol(const MCSymbolCOFF &Symbol);

  void SetSymbolName(COFFSymbol &S);
  void SetSectionName(COFFSection &S);

  bool isUninitializedData(const COFFSection &S);

  // Entity writing methods.
  void WriteFileHeader(const COFF::header &Header);
  void WriteSymbol(const COFFSymbol &S);
  void WriteAuxiliarySymbols(const COFFSymbol::AuxiliarySymbols &S);
  void writeSectionHeaders();
  void WriteRelocation(const COFF::relocation &R);
  uint32_t writeSectionContents(const MCSection &MCSec);
  void writeSection(const COFFSection &Sec);

  void createFileSymbols();
  void setWeakDefaultNames();
  void assignSectionNumbers();
  void assignFileOffsets();
};

WinCOFFObjectWriter::WinCOFFObjectWriter(
    std::unique_ptr<MCWinCOFFObjectTargetWriter> MOTW, raw_pwrite_stream &OS)
    : TargetObjectWriter(std::move(MOTW)),
      ObjWriter(std::make_unique<WinCOFFWriter>(*this, OS,
                                                WinCOFFWriter::AllSections)) {}
WinCOFFObjectWriter::WinCOFFObjectWriter(
    std::unique_ptr<MCWinCOFFObjectTargetWriter> MOTW, raw_pwrite_stream &OS,
    raw_pwrite_stream &DwoOS)
    : TargetObjectWriter(std::move(MOTW)),
      ObjWriter(std::make_unique<WinCOFFWriter>(*this, OS,
                                                WinCOFFWriter::NonDwoOnly)),
      DwoWriter(std::make_unique<WinCOFFWriter>(*this, DwoOS,
                                                WinCOFFWriter::DwoOnly)) {}

static bool isDwoSection(const MCSection &Sec) {
  return Sec.getName().ends_with(".dwo");
}

//------------------------------------------------------------------------------
// Symbol class implementation

// In the case that the name does not fit within 8 bytes, the offset
// into the string table is stored in the last 4 bytes instead, leaving
// the first 4 bytes as 0.
void COFFSymbol::set_name_offset(uint32_t Offset) {
  write32le(Data.Name + 0, 0);
  write32le(Data.Name + 4, Offset);
}

//------------------------------------------------------------------------------
// WinCOFFWriter class implementation

WinCOFFWriter::WinCOFFWriter(WinCOFFObjectWriter &OWriter,
                             raw_pwrite_stream &OS, DwoMode Mode)
    : OWriter(OWriter), W(OS, llvm::endianness::little), Mode(Mode) {
  Header.Machine = OWriter.TargetObjectWriter->getMachine();
  // Some relocations on ARM64 (the 21 bit ADRP relocations) have a slightly
  // limited range for the immediate offset (+/- 1 MB); create extra offset
  // label symbols with regular intervals to allow referencing a
  // non-temporary symbol that is close enough.
  UseOffsetLabels = COFF::isAnyArm64(Header.Machine);
}

COFFSymbol *WinCOFFWriter::createSymbol(StringRef Name) {
  Symbols.push_back(std::make_unique<COFFSymbol>(Name));
  return Symbols.back().get();
}

COFFSymbol *WinCOFFWriter::getOrCreateCOFFSymbol(const MCSymbol &Sym) {
  COFFSymbol *&Ret = SymbolMap[&Sym];
  if (!Ret)
    Ret = createSymbol(Sym.getName());
  return Ret;
}

COFFSymbol *WinCOFFWriter::getOrCreateSecRelSymbol(const MCSymbol &Sym,
                                                   uint64_t Offset) {
  MCSection *TargetSection = &Sym.getSection();
  assert(SectionMap.contains(TargetSection) &&
         "Section must already have been defined in executePostLayoutBinding!");

  COFFSymbol *&Ret = SecRelSymbolMap[{TargetSection, Offset}];
  if (Ret)
    return Ret;

  COFFSection *Section = SectionMap[TargetSection];
  std::string Name;
  // A section the linker resizes may need a label for every value that depends
  // on its layout, so each takes a short name and no string table entry.
  if (TargetSection->isLinkerRelaxable())
    Name = ("$L" + Twine::utohexstr(++SecRelSymbolCount)).str();
  else if (Sym.isTemporary() && !Sym.getName().empty() &&
           Offset == Asm->getSymbolOffset(Sym))
    Name = Sym.getName().str();
  else
    Name =
        (Twine("$L") + Section->Name + "_secrel_" + Twine(++SecRelSymbolCount))
            .str();

  Ret = createSymbol(Name);
  Ret->Section = Section;
  Ret->Data.StorageClass = COFF::IMAGE_SYM_CLASS_LABEL;
  Ret->Data.Value = Offset;
  return Ret;
}

COFFSection *WinCOFFWriter::createSection(StringRef Name) {
  Sections.emplace_back(std::make_unique<COFFSection>(Name));
  return Sections.back().get();
}

static uint32_t getAlignment(const MCSectionCOFF &Sec) {
  switch (Sec.getAlign().value()) {
  case 1:
    return COFF::IMAGE_SCN_ALIGN_1BYTES;
  case 2:
    return COFF::IMAGE_SCN_ALIGN_2BYTES;
  case 4:
    return COFF::IMAGE_SCN_ALIGN_4BYTES;
  case 8:
    return COFF::IMAGE_SCN_ALIGN_8BYTES;
  case 16:
    return COFF::IMAGE_SCN_ALIGN_16BYTES;
  case 32:
    return COFF::IMAGE_SCN_ALIGN_32BYTES;
  case 64:
    return COFF::IMAGE_SCN_ALIGN_64BYTES;
  case 128:
    return COFF::IMAGE_SCN_ALIGN_128BYTES;
  case 256:
    return COFF::IMAGE_SCN_ALIGN_256BYTES;
  case 512:
    return COFF::IMAGE_SCN_ALIGN_512BYTES;
  case 1024:
    return COFF::IMAGE_SCN_ALIGN_1024BYTES;
  case 2048:
    return COFF::IMAGE_SCN_ALIGN_2048BYTES;
  case 4096:
    return COFF::IMAGE_SCN_ALIGN_4096BYTES;
  case 8192:
    return COFF::IMAGE_SCN_ALIGN_8192BYTES;
  }
  llvm_unreachable("unsupported section alignment");
}

/// This function takes a section data object from the assembler
/// and creates the associated COFF section staging object.
void WinCOFFWriter::defineSection(const MCSectionCOFF &MCSec) {
  COFFSection *Section = createSection(MCSec.getName());
  COFFSymbol *Symbol = createSymbol(MCSec.getName());
  Section->Symbol = Symbol;
  SymbolMap[MCSec.getBeginSymbol()] = Symbol;
  Symbol->Section = Section;
  Symbol->Data.StorageClass = COFF::IMAGE_SYM_CLASS_STATIC;

  // Create a COMDAT symbol if needed.
  if (MCSec.getSelection() != COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE) {
    if (const MCSymbol *S = MCSec.getCOMDATSymbol()) {
      COFFSymbol *COMDATSymbol = getOrCreateCOFFSymbol(*S);
      if (COMDATSymbol->Section)
        report_fatal_error("two sections have the same comdat");
      COMDATSymbol->Section = Section;
    }
  }

  // In this case the auxiliary symbol is a Section Definition.
  Symbol->Aux.resize(1);
  Symbol->Aux[0] = {};
  Symbol->Aux[0].AuxType = ATSectionDefinition;
  Symbol->Aux[0].Aux.SectionDefinition.Selection = MCSec.getSelection();

  // Set section alignment.
  Section->Header.Characteristics = MCSec.getCharacteristics();
  Section->Header.Characteristics |= getAlignment(MCSec);

  // Bind internal COFF section to MC section.
  Section->MCSection = &MCSec;
  SectionMap[&MCSec] = Section;

  if (UseOffsetLabels) {
    const uint32_t Interval = 1 << OffsetLabelIntervalBits;
    uint32_t N = 1;
    for (uint32_t Off = Interval, E = Asm->getSectionAddressSize(MCSec);
         Off < E; Off += Interval) {
      auto Name = ("$L" + MCSec.getName() + "_" + Twine(N++)).str();
      COFFSymbol *Label = createSymbol(Name);
      Label->Section = Section;
      Label->Data.StorageClass = COFF::IMAGE_SYM_CLASS_LABEL;
      Label->Data.Value = Off;
      Section->OffsetSymbols.push_back(Label);
    }
  }
}

static uint64_t getSymbolValue(const MCSymbolCOFF &Symbol,
                               const MCAssembler &Asm) {
  if (Symbol.isCommon() && Symbol.isExternal())
    return Symbol.getCommonSize();

  uint64_t Res;
  if (!Asm.getSymbolOffset(Symbol, Res))
    return 0;

  return Res;
}

COFFSymbol *WinCOFFWriter::getLinkedSymbol(const MCSymbol &Symbol) {
  if (!Symbol.isVariable())
    return nullptr;

  const auto *SymRef = dyn_cast<MCSymbolRefExpr>(Symbol.getVariableValue());
  if (!SymRef)
    return nullptr;

  auto &Aliasee = static_cast<const MCSymbolCOFF &>(SymRef->getSymbol());
  if (Aliasee.isUndefined() || Aliasee.isExternal())
    return getOrCreateCOFFSymbol(Aliasee);
  else
    return nullptr;
}

/// This function takes a symbol data object from the assembler
/// and creates the associated COFF symbol staging object.
void WinCOFFWriter::defineSymbol(const MCSymbolCOFF &MCSym) {
  const MCSymbol *Base = Asm->getBaseSymbol(MCSym);
  COFFSection *Sec = nullptr;
  MCSectionCOFF *MCSec = nullptr;
  if (Base && Base->getFragment()) {
    MCSec = static_cast<MCSectionCOFF *>(Base->getFragment()->getParent());
    Sec = SectionMap[MCSec];
  }

  if (Mode == NonDwoOnly && MCSec && isDwoSection(*MCSec))
    return;

  COFFSymbol *Sym = getOrCreateCOFFSymbol(MCSym);
  COFFSymbol *Local = nullptr;
  if (static_cast<const MCSymbolCOFF &>(MCSym)
          .getWeakExternalCharacteristics()) {
    Sym->Data.StorageClass = COFF::IMAGE_SYM_CLASS_WEAK_EXTERNAL;
    Sym->Section = nullptr;

    COFFSymbol *WeakDefault = getLinkedSymbol(MCSym);
    if (!WeakDefault) {
      std::string WeakName = (".weak." + MCSym.getName() + ".default").str();
      WeakDefault = createSymbol(WeakName);
      if (!Sec)
        WeakDefault->Data.SectionNumber = COFF::IMAGE_SYM_ABSOLUTE;
      else
        WeakDefault->Section = Sec;
      WeakDefaults.insert(WeakDefault);
      Local = WeakDefault;
    }

    Sym->Other = WeakDefault;

    // Setup the Weak External auxiliary symbol.
    Sym->Aux.resize(1);
    memset(&Sym->Aux[0], 0, sizeof(Sym->Aux[0]));
    Sym->Aux[0].AuxType = ATWeakExternal;
    Sym->Aux[0].Aux.WeakExternal.TagIndex = 0; // Filled in later
    Sym->Aux[0].Aux.WeakExternal.Characteristics =
        static_cast<const MCSymbolCOFF &>(MCSym)
            .getWeakExternalCharacteristics();
    // Windows Itanium and NT-POSIX bind a weak reference as ELF binds a weak
    // undefined symbol: a strong reference from another object prevails. An
    // absolute value is a definition.
    if (Local && !Sec && !MCSym.isVariable() &&
        getContext().getTargetTriple().isWindowsItaniumABIEnvironment())
      Sym->Aux[0].Aux.WeakExternal.Characteristics =
          COFF::IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY;
  } else {
    if (!Base)
      Sym->Data.SectionNumber = COFF::IMAGE_SYM_ABSOLUTE;
    else
      Sym->Section = Sec;
    Local = Sym;
  }

  if (Local) {
    Local->Data.Value = getSymbolValue(MCSym, *Asm);

    auto &SymbolCOFF = static_cast<const MCSymbolCOFF &>(MCSym);
    Local->Data.Type = SymbolCOFF.getType();
    Local->Data.StorageClass = SymbolCOFF.getClass();

    // If no storage class was specified in the streamer, define it here.
    if (Local->Data.StorageClass == COFF::IMAGE_SYM_CLASS_NULL) {
      bool IsExternal =
          MCSym.isExternal() || (!MCSym.getFragment() && !MCSym.isVariable());

      Local->Data.StorageClass = IsExternal ? COFF::IMAGE_SYM_CLASS_EXTERNAL
                                            : COFF::IMAGE_SYM_CLASS_STATIC;
    }

    // An external function defined in a section states its size in a function
    // definition auxiliary symbol. On a static symbol an auxiliary symbol is a
    // section definition.
    if (SymbolCOFF.getSize() && Sec &&
        Local->Data.StorageClass == COFF::IMAGE_SYM_CLASS_EXTERNAL &&
        Local->Data.Type == COFF::IMAGE_SYM_DTYPE_FUNCTION
                                << COFF::SCT_COMPLEX_TYPE_SHIFT) {
      int64_t Size = 0;
      if (!SymbolCOFF.getSize()->evaluateKnownAbsolute(Size, *Asm))
        getContext().reportError(SymbolCOFF.getSize()->getLoc(),
                                 "size expression must be absolute");
      Local->Aux.resize(1);
      Local->Aux[0] = {};
      Local->Aux[0].AuxType = ATFunctionDefinition;
      Local->Aux[0].Aux.FunctionDefinition.TotalSize = Size;
    }
  }

  Sym->MC = &MCSym;
}

void WinCOFFWriter::SetSectionName(COFFSection &S) {
  if (S.Name.size() <= COFF::NameSize) {
    std::memcpy(S.Header.Name, S.Name.c_str(), S.Name.size());
    return;
  }

  uint64_t StringTableEntry = Strings.getOffset(S.Name);
  if (!COFF::encodeSectionName(S.Header.Name, StringTableEntry))
    report_fatal_error("COFF string table is greater than 64 GB.");
}

void WinCOFFWriter::SetSymbolName(COFFSymbol &S) {
  if (S.Name.size() > COFF::NameSize)
    S.set_name_offset(Strings.getOffset(S.Name));
  else
    std::memcpy(S.Data.Name, S.Name.c_str(), S.Name.size());
}

bool WinCOFFWriter::isUninitializedData(const COFFSection &S) {
  return (S.Header.Characteristics & COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA) !=
         0;
}

//------------------------------------------------------------------------------
// entity writing methods

void WinCOFFWriter::WriteFileHeader(const COFF::header &Header) {
  if (UseBigObj) {
    W.write<uint16_t>(COFF::IMAGE_FILE_MACHINE_UNKNOWN);
    W.write<uint16_t>(0xFFFF);
    W.write<uint16_t>(COFF::BigObjHeader::MinBigObjectVersion);
    W.write<uint16_t>(Header.Machine);
    W.write<uint32_t>(Header.TimeDateStamp);
    W.OS.write(COFF::BigObjMagic, sizeof(COFF::BigObjMagic));
    W.write<uint32_t>(0);
    W.write<uint32_t>(0);
    W.write<uint32_t>(0);
    W.write<uint32_t>(0);
    W.write<uint32_t>(Header.NumberOfSections);
    W.write<uint32_t>(Header.PointerToSymbolTable);
    W.write<uint32_t>(Header.NumberOfSymbols);
  } else {
    W.write<uint16_t>(Header.Machine);
    W.write<uint16_t>(static_cast<int16_t>(Header.NumberOfSections));
    W.write<uint32_t>(Header.TimeDateStamp);
    W.write<uint32_t>(Header.PointerToSymbolTable);
    W.write<uint32_t>(Header.NumberOfSymbols);
    W.write<uint16_t>(Header.SizeOfOptionalHeader);
    W.write<uint16_t>(Header.Characteristics);
  }
}

void WinCOFFWriter::WriteSymbol(const COFFSymbol &S) {
  W.OS.write(S.Data.Name, COFF::NameSize);
  W.write<uint32_t>(S.Data.Value);
  if (UseBigObj)
    W.write<uint32_t>(S.Data.SectionNumber);
  else
    W.write<uint16_t>(static_cast<int16_t>(S.Data.SectionNumber));
  W.write<uint16_t>(S.Data.Type);
  W.OS << char(S.Data.StorageClass);
  W.OS << char(S.Data.NumberOfAuxSymbols);
  WriteAuxiliarySymbols(S.Aux);
}

void WinCOFFWriter::WriteAuxiliarySymbols(
    const COFFSymbol::AuxiliarySymbols &S) {
  for (const AuxSymbol &i : S) {
    switch (i.AuxType) {
    case ATFunctionDefinition:
      W.write<uint32_t>(i.Aux.FunctionDefinition.TagIndex);
      W.write<uint32_t>(i.Aux.FunctionDefinition.TotalSize);
      W.OS.write_zeros((UseBigObj ? COFF::Symbol32Size : COFF::Symbol16Size) -
                       2 * sizeof(uint32_t));
      break;
    case ATWeakExternal:
      W.write<uint32_t>(i.Aux.WeakExternal.TagIndex);
      W.write<uint32_t>(i.Aux.WeakExternal.Characteristics);
      W.OS.write_zeros(sizeof(i.Aux.WeakExternal.unused));
      if (UseBigObj)
        W.OS.write_zeros(COFF::Symbol32Size - COFF::Symbol16Size);
      break;
    case ATFile:
      W.OS.write(reinterpret_cast<const char *>(&i.Aux),
                 UseBigObj ? COFF::Symbol32Size : COFF::Symbol16Size);
      break;
    case ATSectionDefinition:
      W.write<uint32_t>(i.Aux.SectionDefinition.Length);
      W.write<uint16_t>(i.Aux.SectionDefinition.NumberOfRelocations);
      W.write<uint16_t>(i.Aux.SectionDefinition.NumberOfLinenumbers);
      W.write<uint32_t>(i.Aux.SectionDefinition.CheckSum);
      W.write<uint16_t>(static_cast<int16_t>(i.Aux.SectionDefinition.Number));
      W.OS << char(i.Aux.SectionDefinition.Selection);
      W.OS.write_zeros(sizeof(i.Aux.SectionDefinition.unused));
      W.write<uint16_t>(
          static_cast<int16_t>(i.Aux.SectionDefinition.Number >> 16));
      if (UseBigObj)
        W.OS.write_zeros(COFF::Symbol32Size - COFF::Symbol16Size);
      break;
    }
  }
}

// Write the section header.
void WinCOFFWriter::writeSectionHeaders() {
  // Section numbers must be monotonically increasing in the section
  // header, but our Sections array is not sorted by section number,
  // so make a copy of Sections and sort it.
  std::vector<COFFSection *> Arr;
  for (auto &Section : Sections)
    Arr.push_back(Section.get());
  llvm::sort(Arr, [](const COFFSection *A, const COFFSection *B) {
    return A->Number < B->Number;
  });

  for (auto &Section : Arr) {
    if (Section->Number == -1)
      continue;

    COFF::section &S = Section->Header;
    if (Section->Relocations.size() >= 0xffff)
      S.Characteristics |= COFF::IMAGE_SCN_LNK_NRELOC_OVFL;
    W.OS.write(S.Name, COFF::NameSize);
    W.write<uint32_t>(S.VirtualSize);
    W.write<uint32_t>(S.VirtualAddress);
    W.write<uint32_t>(S.SizeOfRawData);
    W.write<uint32_t>(S.PointerToRawData);
    W.write<uint32_t>(S.PointerToRelocations);
    W.write<uint32_t>(S.PointerToLineNumbers);
    W.write<uint16_t>(S.NumberOfRelocations);
    W.write<uint16_t>(S.NumberOfLineNumbers);
    W.write<uint32_t>(S.Characteristics);
  }
}

void WinCOFFWriter::WriteRelocation(const COFF::relocation &R) {
  W.write<uint32_t>(R.VirtualAddress);
  W.write<uint32_t>(R.SymbolTableIndex);
  W.write<uint16_t>(R.Type);
}

// Write MCSec's contents. What this function does is essentially
// "Asm.writeSectionData(&MCSec)", but it's a bit complicated
// because it needs to compute a CRC.
uint32_t WinCOFFWriter::writeSectionContents(const MCSection &MCSec) {
  // Save the contents of the section to a temporary buffer, we need this
  // to CRC the data before we dump it into the object file.
  SmallVector<char, 128> Buf;
  raw_svector_ostream VecOS(Buf);
  Asm->writeSectionData(VecOS, &MCSec);

  // Write the section contents to the object file.
  W.OS << Buf;

  // Calculate our CRC with an initial value of '0', this is not how
  // JamCRC is specified but it aligns with the expected output.
  JamCRC JC(/*Init=*/0);
  JC.update(ArrayRef(reinterpret_cast<uint8_t *>(Buf.data()), Buf.size()));
  return JC.getCRC();
}

void WinCOFFWriter::writeSection(const COFFSection &Sec) {
  if (Sec.Number == -1)
    return;

  // Write the section contents.
  if (Sec.Header.PointerToRawData != 0) {
    assert(W.OS.tell() == Sec.Header.PointerToRawData &&
           "Section::PointerToRawData is insane!");

    uint32_t CRC = writeSectionContents(*Sec.MCSection);

    // Update the section definition auxiliary symbol to record the CRC.
    COFFSymbol::AuxiliarySymbols &AuxSyms = Sec.Symbol->Aux;
    assert(AuxSyms.size() == 1 && AuxSyms[0].AuxType == ATSectionDefinition);
    AuxSymbol &SecDef = AuxSyms[0];
    SecDef.Aux.SectionDefinition.CheckSum = CRC;
  } else if (isUninitializedData(Sec)) {
    // Error if fixups or non-zero bytes are present.
    writeSectionContents(*Sec.MCSection);
  }

  // Write relocations for this section.
  if (Sec.Relocations.empty()) {
    assert(Sec.Header.PointerToRelocations == 0 &&
           "Section::PointerToRelocations is insane!");
    return;
  }

  assert(W.OS.tell() == Sec.Header.PointerToRelocations &&
         "Section::PointerToRelocations is insane!");

  if (Sec.Relocations.size() >= 0xffff) {
    // In case of overflow, write actual relocation count as first
    // relocation. Including the synthetic reloc itself (+ 1).
    COFF::relocation R;
    R.VirtualAddress = Sec.Relocations.size() + 1;
    R.SymbolTableIndex = 0;
    R.Type = 0;
    WriteRelocation(R);
  }

  for (const auto &Relocation : Sec.Relocations)
    WriteRelocation(Relocation.Data);
}

// Create .file symbols.
void WinCOFFWriter::createFileSymbols() {
  for (const std::pair<std::string, size_t> &It : OWriter.getFileNames()) {
    // round up to calculate the number of auxiliary symbols required
    const std::string &Name = It.first;
    unsigned SymbolSize = UseBigObj ? COFF::Symbol32Size : COFF::Symbol16Size;
    unsigned Count = (Name.size() + SymbolSize - 1) / SymbolSize;

    COFFSymbol *File = createSymbol(".file");
    File->Data.SectionNumber = COFF::IMAGE_SYM_DEBUG;
    File->Data.StorageClass = COFF::IMAGE_SYM_CLASS_FILE;
    File->Aux.resize(Count);

    unsigned Offset = 0;
    unsigned Length = Name.size();
    for (auto &Aux : File->Aux) {
      Aux.AuxType = ATFile;

      if (Length > SymbolSize) {
        memcpy(&Aux.Aux, Name.c_str() + Offset, SymbolSize);
        Length = Length - SymbolSize;
      } else {
        memcpy(&Aux.Aux, Name.c_str() + Offset, Length);
        memset((char *)&Aux.Aux + Length, 0, SymbolSize - Length);
        break;
      }

      Offset += SymbolSize;
    }
  }
}

void WinCOFFWriter::setWeakDefaultNames() {
  if (WeakDefaults.empty())
    return;

  // If multiple object files use a weak symbol (either with a regular
  // defined default, or an absolute zero symbol as default), the defaults
  // cause duplicate definitions unless their names are made unique. Look
  // for a defined extern symbol, that isn't comdat - that should be unique
  // unless there are other duplicate definitions. And if none is found,
  // allow picking a comdat symbol, as that's still better than nothing.

  COFFSymbol *Unique = nullptr;
  for (bool AllowComdat : {false, true}) {
    for (auto &Sym : Symbols) {
      // Don't include the names of the defaults themselves
      if (WeakDefaults.count(Sym.get()))
        continue;
      // Only consider external symbols
      if (Sym->Data.StorageClass != COFF::IMAGE_SYM_CLASS_EXTERNAL)
        continue;
      // Only consider symbols defined in a section or that are absolute
      if (!Sym->Section && Sym->Data.SectionNumber != COFF::IMAGE_SYM_ABSOLUTE)
        continue;
      if (!AllowComdat && Sym->Section &&
          Sym->Section->Header.Characteristics & COFF::IMAGE_SCN_LNK_COMDAT)
        continue;
      Unique = Sym.get();
      break;
    }
    if (Unique)
      break;
  }
  // If we didn't find any unique symbol to use for the names, just skip this.
  if (!Unique)
    return;
  for (auto *Sym : WeakDefaults) {
    Sym->Name.append(".");
    Sym->Name.append(Unique->Name);
  }
}

static bool isAssociative(const COFFSection &Section) {
  return Section.Symbol->Aux[0].Aux.SectionDefinition.Selection ==
         COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE;
}

void WinCOFFWriter::assignSectionNumbers() {
  size_t I = 1;
  auto Assign = [&](COFFSection &Section) {
    Section.Number = I;
    Section.Symbol->Data.SectionNumber = I;
    Section.Symbol->Aux[0].Aux.SectionDefinition.Number = I;
    ++I;
  };

  // An object for Windows Itanium or NT-POSIX has .drectve as its first
  // section.
  bool DrectveFirst =
      getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment();
  for (const std::unique_ptr<COFFSection> &Section : Sections)
    if (DrectveFirst && Section->Name == ".drectve")
      Assign(*Section);

  // Although it is not explicitly requested by the Microsoft COFF spec,
  // we should avoid emitting forward associative section references,
  // because MSVC link.exe as of 2017 cannot handle that.
  for (const std::unique_ptr<COFFSection> &Section : Sections)
    if (!isAssociative(*Section) && !Section->Number)
      Assign(*Section);
  for (const std::unique_ptr<COFFSection> &Section : Sections)
    if (isAssociative(*Section))
      Assign(*Section);
}

// Assign file offsets to COFF object file structures.
void WinCOFFWriter::assignFileOffsets() {
  unsigned Offset = W.OS.tell();

  Offset += UseBigObj ? COFF::Header32Size : COFF::Header16Size;
  Offset += COFF::SectionSize * Header.NumberOfSections;

  for (const auto &Section : *Asm) {
    COFFSection *Sec = SectionMap[&Section];

    if (!Sec || Sec->Number == -1)
      continue;

    Sec->Header.SizeOfRawData = Asm->getSectionAddressSize(Section);

    if (!isUninitializedData(*Sec)) {
      Sec->Header.PointerToRawData = Offset;
      Offset += Sec->Header.SizeOfRawData;
    }

    if (!Sec->Relocations.empty()) {
      bool RelocationsOverflow = Sec->Relocations.size() >= 0xffff;

      if (RelocationsOverflow) {
        // Signal overflow by setting NumberOfRelocations to max value. Actual
        // size is found in reloc #0. Microsoft tools understand this.
        Sec->Header.NumberOfRelocations = 0xffff;
      } else {
        Sec->Header.NumberOfRelocations = Sec->Relocations.size();
      }
      Sec->Header.PointerToRelocations = Offset;

      if (RelocationsOverflow) {
        // Reloc #0 will contain actual count, so make room for it.
        Offset += COFF::RelocationSize;
      }

      Offset += COFF::RelocationSize * Sec->Relocations.size();

      for (auto &Relocation : Sec->Relocations) {
        // A relocation that names no symbol holds a number in its index.
        if (!Relocation.Symb)
          continue;
        assert(Relocation.Symb->getIndex() != -1);
        if (Header.Machine != COFF::IMAGE_FILE_MACHINE_R4000 ||
            Relocation.Data.Type != COFF::IMAGE_REL_MIPS_PAIR) {
          Relocation.Data.SymbolTableIndex = Relocation.Symb->getIndex();
        }
      }
    }

    assert(Sec->Symbol->Aux.size() == 1 &&
           "Section's symbol must have one aux!");
    AuxSymbol &Aux = Sec->Symbol->Aux[0];
    assert(Aux.AuxType == ATSectionDefinition &&
           "Section's symbol's aux symbol must be a Section Definition!");
    Aux.Aux.SectionDefinition.Length = Sec->Header.SizeOfRawData;
    Aux.Aux.SectionDefinition.NumberOfRelocations =
        Sec->Header.NumberOfRelocations;
    Aux.Aux.SectionDefinition.NumberOfLinenumbers =
        Sec->Header.NumberOfLineNumbers;
  }

  Header.PointerToSymbolTable = Offset;
}

void WinCOFFWriter::reset() {
  memset(&Header, 0, sizeof(Header));
  Header.Machine = OWriter.TargetObjectWriter->getMachine();
  Sections.clear();
  Symbols.clear();
  Strings.clear();
  SectionMap.clear();
  SymbolMap.clear();
  SecRelSymbolMap.clear();
  WeakDefaults.clear();
  SecRelSymbolCount = 0;
}

void WinCOFFWriter::executePostLayoutBinding() {
  // @comp.id marks an object that LLVM wrote for Windows Itanium or NT-POSIX,
  // whatever the object was written from. A definition of it in the assembly
  // is this symbol and does not change it.
  const MCSymbol *Mark = nullptr;
  if (Mode != DwoOnly &&
      getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
    COFFSymbol *CompId = createSymbol("@comp.id");
    CompId->Data.Value = COFF::LLVMCompId;
    CompId->Data.SectionNumber = COFF::IMAGE_SYM_ABSOLUTE;
    CompId->Data.StorageClass = COFF::IMAGE_SYM_CLASS_STATIC;
    if ((Mark = getContext().lookupSymbol("@comp.id")))
      SymbolMap[Mark] = CompId;
  }

  // "Define" each section & symbol. This creates section & symbol
  // entries in the staging area.
  for (const auto &Section : *Asm) {
    if ((Mode == NonDwoOnly && isDwoSection(Section)) ||
        (Mode == DwoOnly && !isDwoSection(Section)))
      continue;
    defineSection(static_cast<const MCSectionCOFF &>(Section));
  }

  if (Mode != DwoOnly) {
    for (const MCSymbol &Symbol : Asm->symbols()) {
      auto &Sym = static_cast<const MCSymbolCOFF &>(Symbol);
      // Define non-temporary or temporary static (private-linkage) symbols
      if (&Symbol != Mark && (!Sym.isTemporary() ||
                              Sym.getClass() == COFF::IMAGE_SYM_CLASS_STATIC))
        defineSymbol(Sym);
    }

    // Such an object lists the functions whose address it takes, which
    // @feat.00 states. A definition of it in the assembly keeps its other
    // bits.
    if (getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
      COFFSymbol *Feat =
          SymbolMap.lookup(getContext().lookupSymbol("@feat.00"));
      if (!Feat) {
        Feat = createSymbol("@feat.00");
        Feat->Data.SectionNumber = COFF::IMAGE_SYM_ABSOLUTE;
        Feat->Data.StorageClass = COFF::IMAGE_SYM_CLASS_STATIC;
      }
      Feat->Data.Value |= COFF::Feat00Flags::GuardCF;
    }
  }

  UseBigObj = Sections.size() > COFF::MaxNumberOfSections16;
  Header.NumberOfSections = Sections.size();
  Header.NumberOfSymbols = 0;
  if (Sections.size() > INT32_MAX)
    report_fatal_error(
        "PE COFF object files can't have more than 2147483647 sections");

  assignSectionNumbers();
}

void WinCOFFWriter::recordRelocation(const MCFragment &F, const MCFixup &Fixup,
                                     MCValue Target, uint64_t &FixedValue) {
  // A fixup may state its relocation type, as .reloc does. A type that names
  // no symbol holds a number in its index.
  bool IsLiteral = mc::isRelocRelocation(Fixup.getKind());
  uint16_t Literal = getCOFFRelocType(Fixup.getKind());
  if (IsLiteral && COFF::relocationNamesNoSymbol(Literal)) {
    COFFRelocation Reloc;
    Reloc.Data.VirtualAddress = Asm->getFragmentOffset(F) + Fixup.getOffset();
    Reloc.Data.SymbolTableIndex = Target.getConstant();
    Reloc.Data.Type = Literal;
    SectionMap[F.getParent()]->Relocations.push_back(Reloc);
    return;
  }
  if (IsLiteral && !Target.getAddSym())
    return getContext().reportError(Fixup.getLoc(),
                                    "relocation must name a symbol");
  assert(Target.getAddSym() && "Relocation must reference a symbol!");

  const MCSymbol &A = *Target.getAddSym();
  if (!A.isRegistered()) {
    getContext().reportError(Fixup.getLoc(), Twine("symbol '") + A.getName() +
                                                 "' can not be undefined");
    return;
  }
  if (A.isTemporary() && A.isUndefined()) {
    getContext().reportError(Fixup.getLoc(), Twine("assembler label '") +
                                                 A.getName() +
                                                 "' can not be undefined");
    return;
  }

  MCSection *MCSec = F.getParent();

  // Mark this symbol as requiring an entry in the symbol table.
  assert(SectionMap.contains(MCSec) &&
         "Section must already have been defined in executePostLayoutBinding!");

  COFFSection *Sec = SectionMap[MCSec];
  if (const MCSymbol *B = Target.getSubSym()) {
    if (!B->getFragment()) {
      getContext().reportError(
          Fixup.getLoc(),
          Twine("symbol '") + B->getName() +
              "' can not be undefined in a subtraction expression");
      return;
    }

    // Offset of the symbol in the section
    int64_t OffsetOfB = Asm->getSymbolOffset(*B);

    // CodeView keeps its compact form, which the linker remaps where it
    // resizes code: a distance in such code is as assembled.
    if (MCSec->getName() == ".debug$S" && A.isInSection() &&
        &A.getSection() == &B->getSection() &&
        A.getSection().isLinkerRelaxable()) {
      FixedValue = Asm->getSymbolOffset(A) - OffsetOfB + Target.getConstant();
      return;
    }

    // REL32 below writes A - B only when B lies in the field's section and
    // the linker resizes nothing between B and the field. Otherwise, in a
    // section the linker resizes that also holds A, and where the fixup
    // states its type, A - B is a span.
    if (IsLiteral || &B->getSection() != MCSec ||
        (MCSec->isLinkerRelaxable() && A.isInSection() &&
         &A.getSection() == MCSec) ||
        !Asm->getBackend().isPCRelFixupResolved(B, F))
      return recordSpan(F, Fixup, A, *B, Target.getConstant(), FixedValue);

    // Offset of the relocation in the section
    int64_t OffsetOfRelocation = Asm->getFragmentOffset(F) + Fixup.getOffset();

    FixedValue = (OffsetOfRelocation - OffsetOfB) + Target.getConstant();
  } else {
    FixedValue = Target.getConstant();
  }

  COFFRelocation Reloc;

  Reloc.Data.SymbolTableIndex = 0;
  Reloc.Data.VirtualAddress = Asm->getFragmentOffset(F);
  Reloc.Data.Type = IsLiteral ? Literal
                              : OWriter.TargetObjectWriter->getRelocType(
                                    getContext(), Target, Fixup,
                                    Target.getSubSym(), Asm->getBackend());

  // The linker may resize the code at such a type, so the assembler must
  // have ended a fragment with it; a marker's site is the streamer's to end.
  // It keeps the length of an AMD64 site in a section whose code it does not
  // move, which is one that it does not resize or that states spans.
  assert(
      !(!IsLiteral && !Target.getSubSym() && !F.isLinkerRelaxable() &&
        (Header.Machine != COFF::IMAGE_FILE_MACHINE_AMD64 ||
         (MCSec->isLinkerRelaxable() && !OWriter.statesSpans(*MCSec))) &&
        getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment() &&
        COFF::relocationMayResizeCode(Header.Machine, Reloc.Data.Type,
                                      A.getName())) &&
      "the linker may resize code that the assembler did not state as "
      "resizable");

  bool IsArm64SecRel12 = false;
  if (COFF::isAnyArm64(Header.Machine)) {
    switch (Reloc.Data.Type) {
    case COFF::IMAGE_REL_ARM64_SECREL_HIGH12A:
    case COFF::IMAGE_REL_ARM64_SECREL_LOW12A:
    case COFF::IMAGE_REL_ARM64_SECREL_LOW12L:
      IsArm64SecRel12 = true;
      break;
    }
  }

  bool NeedsSecRelSymbol = false;
  bool UseSectionSymbol = A.isTemporary() && !SymbolMap.lookup(&A);
  // In a section the linker only compacts, an offset from the section's
  // symbol names its point: the linker maps the offset that an absolute field
  // holds, and a span states the one in any other field of 32 bits.
  bool IsAbsolute =
      Header.Machine == COFF::IMAGE_FILE_MACHINE_AMD64 &&
      is_contained({COFF::IMAGE_REL_AMD64_ADDR64, COFF::IMAGE_REL_AMD64_ADDR32,
                    COFF::IMAGE_REL_AMD64_ADDR32NB,
                    COFF::IMAGE_REL_AMD64_SECREL,
                    COFF::IMAGE_REL_AMD64_SECTION},
                   Reloc.Data.Type);
  bool ByOffset =
      UseSectionSymbol && !IsLiteral && A.isInSection() &&
      OWriter.statesSpans(A.getSection()) &&
      (IsAbsolute ||
       Asm->getBackend().getFixupKindInfo(Fixup.getKind()).TargetSize == 32);
  uint64_t SecRelSymbolOffset = 0;
  if (IsArm64SecRel12 && A.isInSection()) {
    uint64_t SecRelFixedValue = FixedValue;
    if (UseSectionSymbol)
      SecRelFixedValue += Asm->getSymbolOffset(A);
    NeedsSecRelSymbol = !isUInt<12>(SecRelFixedValue);
    SecRelSymbolOffset = Asm->getSymbolOffset(A) + FixedValue;
  }

  if (NeedsSecRelSymbol) {
    if (!isUInt<32>(SecRelSymbolOffset)) {
      getContext().reportError(Fixup.getLoc(),
                               "relocation addend out of range");
      return;
    }
    Reloc.Symb = getOrCreateSecRelSymbol(A, SecRelSymbolOffset);
    FixedValue = 0;
  } else if (!ByOffset && A.isInSection() &&
             A.getSection().isLinkerRelaxable() &&
             (UseSectionSymbol || IsLiteral ||
              (Fixup.isPCRel() &&
               OWriter.isSymbolRefDifferenceFullyResolvedImpl(A, F, false,
                                                              true)))) {
    // The linker moves a label with the code it resizes; an offset from the
    // section's start would not follow. A stated type marks a place for a
    // point of this object, and a PC-relative field that the assembler
    // resolved names one, which is a label whatever the symbol resolves to.
    Reloc.Symb = getOrCreateSecRelSymbol(A, Asm->getSymbolOffset(A));
  } else if (UseSectionSymbol && COFF::isAnyArm64(Header.Machine) &&
             Reloc.Data.Type == COFF::IMAGE_REL_ARM64_BRANCH26) {
    // The field of an ARM64 branch holds no offset from a symbol, so a target
    // in another section, a part of the branch's function there, is a label.
    Reloc.Symb = getOrCreateSecRelSymbol(A, Asm->getSymbolOffset(A));
  } else if (UseSectionSymbol) {
    MCSection *TargetSection = &A.getSection();
    assert(
        SectionMap.contains(TargetSection) &&
        "Section must already have been defined in executePostLayoutBinding!");
    COFFSection *Section = SectionMap[TargetSection];
    Reloc.Symb = Section->Symbol;
    FixedValue += Asm->getSymbolOffset(A);
    // Technically, we should do the final adjustments of FixedValue (below)
    // before picking an offset symbol, otherwise we might choose one which
    // is slightly too far away. The relocations where it really matters
    // (arm64 adrp relocations) don't get any offset though.
    if (UseOffsetLabels && !Section->OffsetSymbols.empty()) {
      uint64_t LabelIndex = FixedValue >> OffsetLabelIntervalBits;
      if (LabelIndex > 0) {
        if (LabelIndex <= Section->OffsetSymbols.size())
          Reloc.Symb = Section->OffsetSymbols[LabelIndex - 1];
        else
          Reloc.Symb = Section->OffsetSymbols.back();
        FixedValue -= Reloc.Symb->Data.Value;
      }
    }
  } else {
    assert(
        SymbolMap.contains(&A) &&
        "Symbol must already have been defined in executePostLayoutBinding!");
    Reloc.Symb = SymbolMap[&A];
  }

  ++Reloc.Symb->Relocations;

  Reloc.Data.VirtualAddress += Fixup.getOffset();

  // The *_REL32 relocations are relative to the end of the relocation,
  // not to the start; AMD64's REL32_1 to REL32_5 to 1 to 5 bytes after it, as
  // is its fused test, to the byte of immediate after it. So are the
  // PC-relative LLVM types, whose field has four bytes or, for JUMP8, one. An
  // AMD64 fused read's field holds no addend, and what its fixup adds is
  // written as it is.
  if (Header.Machine == COFF::IMAGE_FILE_MACHINE_AMD64 &&
      Reloc.Data.Type >= COFF::IMAGE_REL_AMD64_REL32 &&
      Reloc.Data.Type <= COFF::IMAGE_REL_AMD64_REL32_5)
    FixedValue += 4 + Reloc.Data.Type - COFF::IMAGE_REL_AMD64_REL32;
  else if (Header.Machine == COFF::IMAGE_FILE_MACHINE_AMD64 &&
           COFF::isRel32Insn(Reloc.Data.Type))
    FixedValue += 4 + (Reloc.Data.Type & 15);
  else if (Header.Machine == COFF::IMAGE_FILE_MACHINE_AMD64 &&
           Reloc.Data.Type == COFF::IMAGE_REL_LLVM_BRANCH_TAIL)
    FixedValue += 4;
  else if (Header.Machine == COFF::IMAGE_FILE_MACHINE_AMD64 &&
           Reloc.Data.Type == COFF::IMAGE_REL_LLVM_FUSED_TEST)
    FixedValue += 5;
  else if ((Header.Machine == COFF::IMAGE_FILE_MACHINE_I386 &&
            Reloc.Data.Type == COFF::IMAGE_REL_I386_REL32) ||
           (Header.Machine == COFF::IMAGE_FILE_MACHINE_ARMNT &&
            Reloc.Data.Type == COFF::IMAGE_REL_ARM_REL32) ||
           (COFF::isAnyArm64(Header.Machine) &&
            Reloc.Data.Type == COFF::IMAGE_REL_ARM64_REL32) ||
           Reloc.Data.Type == COFF::IMAGE_REL_LLVM_REL32_CALL ||
           (Reloc.Data.Type >= COFF::IMAGE_REL_LLVM_BRANCH32 &&
            Reloc.Data.Type <= COFF::IMAGE_REL_LLVM_LEA_OFFSET_REX2) ||
           (Header.Machine == COFF::IMAGE_FILE_MACHINE_AMD64 &&
            (Reloc.Data.Type == COFF::IMAGE_REL_LLVM_FUSED_READ ||
             Reloc.Data.Type == COFF::IMAGE_REL_LLVM_FUSED_READ_REX2 ||
             Reloc.Data.Type == COFF::IMAGE_REL_LLVM_GOTTPOFF32)))
    FixedValue += Reloc.Data.Type == COFF::IMAGE_REL_LLVM_JUMP8 ? 1 : 4;

  if (Header.Machine == COFF::IMAGE_FILE_MACHINE_ARMNT) {
    switch (Reloc.Data.Type) {
    case COFF::IMAGE_REL_ARM_ABSOLUTE:
    case COFF::IMAGE_REL_ARM_ADDR32:
    case COFF::IMAGE_REL_ARM_ADDR32NB:
    case COFF::IMAGE_REL_ARM_TOKEN:
    case COFF::IMAGE_REL_ARM_SECTION:
    case COFF::IMAGE_REL_ARM_SECREL:
      break;
    case COFF::IMAGE_REL_ARM_BRANCH11:
    case COFF::IMAGE_REL_ARM_BLX11:
    // IMAGE_REL_ARM_BRANCH11 and IMAGE_REL_ARM_BLX11 are only used for
    // pre-ARMv7, which implicitly rules it out of ARMNT (it would be valid
    // for Windows CE).
    case COFF::IMAGE_REL_ARM_BRANCH24:
    case COFF::IMAGE_REL_ARM_BLX24:
    case COFF::IMAGE_REL_ARM_MOV32A:
      // IMAGE_REL_ARM_BRANCH24, IMAGE_REL_ARM_BLX24, IMAGE_REL_ARM_MOV32A are
      // only used for ARM mode code, which is documented as being unsupported
      // by Windows on ARM.  Empirical proof indicates that masm is able to
      // generate the relocations however the rest of the MSVC toolchain is
      // unable to handle it.
      llvm_unreachable("unsupported relocation");
      break;
    case COFF::IMAGE_REL_ARM_MOV32T:
      break;
    case COFF::IMAGE_REL_ARM_BRANCH20T:
    case COFF::IMAGE_REL_ARM_BRANCH24T:
    case COFF::IMAGE_REL_ARM_BLX23T:
      // IMAGE_REL_BRANCH20T, IMAGE_REL_ARM_BRANCH24T, IMAGE_REL_ARM_BLX23T all
      // perform a 4 byte adjustment to the relocation.  Relative branches are
      // offset by 4 on ARM, however, because there is no RELA relocations, all
      // branches are offset by 4.
      FixedValue = FixedValue + 4;
      break;
    }
  }

  // The fixed value never makes sense for section indices, ignore it.
  if (Fixup.getKind() == FK_SecRel_2)
    FixedValue = 0;

  if (ByOffset && !IsAbsolute)
    OWriter.addSpan(F, Fixup, A.getSection(), COFF::SpanAddend32,
                    FixedValue - Asm->getSymbolOffset(A));

  if (OWriter.TargetObjectWriter->recordRelocation(Fixup)) {
    Sec->Relocations.push_back(Reloc);
    if (Header.Machine == COFF::IMAGE_FILE_MACHINE_R4000 &&
        (Reloc.Data.Type == COFF::IMAGE_REL_MIPS_REFHI ||
         Reloc.Data.Type == COFF::IMAGE_REL_MIPS_SECRELHI)) {
      // IMAGE_REL_MIPS_REFHI and IMAGE_REL_MIPS_SECRELHI *must*
      // be followed by IMAGE_REL_MIPS_PAIR
      auto RelocPair = Reloc;
      RelocPair.Data.Type = COFF::IMAGE_REL_MIPS_PAIR;
      Sec->Relocations.push_back(RelocPair);
    }
  }
}

// Writes A + Constant - B as a span type, or the type the fixup states, naming
// A and PAIR naming B, at the field. Both are local, as linkers resolve an
// external one to a thunk, and B lies in A's section or in one associative to
// it, so that B lives while A does.
void WinCOFFWriter::recordSpan(const MCFragment &F, const MCFixup &Fixup,
                               const MCSymbol &A, const MCSymbol &B,
                               int64_t Constant, uint64_t &FixedValue) {
  // x86-64's 32-bit span is the one link.exe applies. The other widths, and
  // every width on AArch64, are types of Windows Itanium and NT-POSIX objects.
  bool IsAMD64 = Header.Machine == COFF::IMAGE_FILE_MACHINE_AMD64;
  unsigned Type = COFF::IMAGE_REL_AMD64_ABSOLUTE;
  if (mc::isRelocRelocation(Fixup.getKind()))
    Type = getCOFFRelocType(Fixup.getKind());
  else if (IsAMD64 &&
           Asm->getBackend().getFixupKindInfo(Fixup.getKind()).TargetSize == 32)
    Type = COFF::IMAGE_REL_AMD64_SSPAN32;
  else if ((IsAMD64 || Header.Machine == COFF::IMAGE_FILE_MACHINE_ARM64) &&
           getContext()
               .getTargetTriple()
               .isWindowsItaniumOrNTPOSIXEnvironment()) {
    // The operand of a DWARF advance is unsigned; a data difference is signed.
    bool IsAdvance = F.getKind() == MCFragment::FT_Dwarf ||
                     F.getKind() == MCFragment::FT_DwarfFrame;
    switch (Fixup.getKind()) {
    case FK_Data_1:
      Type =
          IsAdvance ? COFF::IMAGE_REL_LLVM_USPAN8 : COFF::IMAGE_REL_LLVM_SSPAN8;
      break;
    case FK_Data_2:
      Type = IsAdvance ? COFF::IMAGE_REL_LLVM_USPAN16
                       : COFF::IMAGE_REL_LLVM_SSPAN16;
      break;
    case FK_Data_4:
      Type = COFF::IMAGE_REL_LLVM_SSPAN32;
      break;
    case FK_Data_8:
      Type = COFF::IMAGE_REL_LLVM_SSPAN64;
      break;
    case FK_Data_leb128:
      Type = F.isLEBSigned() ? COFF::IMAGE_REL_LLVM_SSPAN_SLEB
                             : COFF::IMAGE_REL_LLVM_SSPAN_ULEB;
      break;
    }
  }
  if (Type == COFF::IMAGE_REL_AMD64_ABSOLUTE || Fixup.isPCRel())
    return getContext().reportError(
        Fixup.getLoc(), "Cannot represent a difference across sections");
  // The assembler resolved a difference within the field's section to points
  // of this object, which labels mark where a symbol is external.
  auto IsExternal = [](const MCSymbol &S) {
    return static_cast<const MCSymbolCOFF &>(S).isExternal();
  };
  if (!A.isInSection() ||
      ((IsExternal(A) || IsExternal(B)) &&
       (&A.getSection() != F.getParent() || &B.getSection() != F.getParent())))
    return getContext().reportError(
        Fixup.getLoc(), "cross-section difference needs local symbols");
  auto *BSec = static_cast<const MCSectionCOFF *>(&B.getSection());
  while (BSec != &A.getSection() &&
         BSec->getSelection() == COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE)
    BSec = static_cast<const MCSectionCOFF *>(
        &BSec->getCOMDATSymbol()->getSection());
  if (BSec != &A.getSection())
    return getContext().reportError(
        Fixup.getLoc(),
        "cross-section difference needs B in A's section or in one "
        "associative to it");
  // A span has no addend: a label at A + Constant carries it or, where that
  // lies outside A's section, one at B - Constant.
  uint64_t OffsetOfA = Asm->getSymbolOffset(A);
  uint64_t OffsetOfB = Asm->getSymbolOffset(B);
  if (OffsetOfA + Constant <= Asm->getSectionAddressSize(A.getSection()))
    OffsetOfA += Constant;
  else if (OffsetOfB - Constant <= Asm->getSectionAddressSize(B.getSection()))
    OffsetOfB -= Constant;
  else
    return getContext().reportError(
        Fixup.getLoc(),
        "cross-section difference puts its labels outside their sections");
  // A section the linker only compacts states a distance in it by B's
  // offset, without labels.
  if (const uint16_t *T = find(COFF::SpanTypes, Type);
      T != std::end(COFF::SpanTypes) && &A.getSection() == &B.getSection() &&
      OWriter.statesSpans(A.getSection())) {
    OWriter.addSpan(F, Fixup, A.getSection(),
                    COFF::SpanData + (T - COFF::SpanTypes), OffsetOfB);
    FixedValue = OffsetOfA - OffsetOfB;
    return;
  }
  auto Label = [&](const MCSymbol &S, uint64_t Offset) {
    COFFSymbol *Sym = SymbolMap.lookup(&S);
    Sym = Sym && Offset == Asm->getSymbolOffset(S) && !IsExternal(S)
              ? Sym
              : getOrCreateSecRelSymbol(S, Offset);
    ++Sym->Relocations;
    return Sym;
  };
  COFFRelocation Span;
  Span.Data.VirtualAddress = Asm->getFragmentOffset(F) + Fixup.getOffset();
  Span.Data.Type = Type;
  Span.Symb = Label(A, OffsetOfA);
  COFFRelocation Pair = Span;
  Pair.Data.Type =
      IsAMD64 ? COFF::IMAGE_REL_AMD64_PAIR : COFF::IMAGE_REL_ARM64_PAIR;
  Pair.Symb = Label(B, OffsetOfB);
  COFFSection *Sec = SectionMap[F.getParent()];
  Sec->Relocations.push_back(Span);
  Sec->Relocations.push_back(Pair);
  // The linker overwrites the field; it holds A - B where that is known.
  FixedValue = &A.getSection() == &B.getSection() ? OffsetOfA - OffsetOfB : 0;
}

static std::time_t getTime() {
  std::time_t Now = time(nullptr);
  if (Now < 0 || !isUInt<32>(Now))
    return UINT32_MAX;
  return Now;
}

uint64_t WinCOFFWriter::writeObject() {
  uint64_t StartOffset = W.OS.tell();

  setWeakDefaultNames();
  if (Mode != DwoOnly)
    createFileSymbols();

  // The link records name the four places of a dispatch through a jump table
  // by symbols of this object: a label, where a place has no symbol or one
  // that another object may define.
  SmallVector<COFFSymbol *, 0> SwitchLabels;
  if (getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment())
    for (const WinCOFFObjectWriter::SwitchTable &Table : OWriter.SwitchTables)
      for (const MCSymbol *S : Table.Symbols) {
        if (!S->isInSection() || !SectionMap.contains(&S->getSection())) {
          getContext().reportError(SMLoc(),
                                   "switch table names '" + S->getName() +
                                       "', which this object does not define");
          return 0;
        }
        COFFSymbol *Sym = SymbolMap.lookup(S);
        SwitchLabels.push_back(
            Sym && !static_cast<const MCSymbolCOFF *>(S)->isExternal()
                ? Sym
                : getOrCreateSecRelSymbol(*S, Asm->getSymbolOffset(*S)));
      }

  for (auto &Symbol : Symbols) {
    // Update section number & offset for symbols that have them.
    if (Symbol->Section)
      Symbol->Data.SectionNumber = Symbol->Section->Number;
    Symbol->setIndex(Header.NumberOfSymbols++);
    // Update auxiliary symbol info.
    Symbol->Data.NumberOfAuxSymbols = Symbol->Aux.size();
    Header.NumberOfSymbols += Symbol->Data.NumberOfAuxSymbols;
  }

  // Build string table.
  for (const auto &S : Sections)
    if (S->Name.size() > COFF::NameSize)
      Strings.add(S->Name);
  for (const auto &S : Symbols)
    if (S->Name.size() > COFF::NameSize)
      Strings.add(S->Name);
  Strings.finalize();

  // Set names.
  for (const auto &S : Sections)
    SetSectionName(*S);
  for (auto &S : Symbols)
    SetSymbolName(*S);

  // Fixup weak external references.
  for (auto &Symbol : Symbols) {
    if (Symbol->Other) {
      assert(Symbol->getIndex() != -1);
      assert(Symbol->Aux.size() == 1 && "Symbol must contain one aux symbol!");
      assert(Symbol->Aux[0].AuxType == ATWeakExternal &&
             "Symbol's aux symbol must be a Weak External!");
      Symbol->Aux[0].Aux.WeakExternal.TagIndex = Symbol->Other->getIndex();
    }
  }

  // Fixup associative COMDAT sections.
  for (auto &Section : Sections) {
    if (Section->Symbol->Aux[0].Aux.SectionDefinition.Selection !=
        COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE)
      continue;

    const MCSectionCOFF &MCSec = *Section->MCSection;
    const MCSymbol *AssocMCSym = MCSec.getCOMDATSymbol();
    assert(AssocMCSym);

    // It's an error to try to associate with an undefined symbol or a symbol
    // without a section.
    if (!AssocMCSym->isInSection()) {
      getContext().reportError(
          SMLoc(), Twine("cannot make section ") + MCSec.getName() +
                       Twine(" associative with sectionless symbol ") +
                       AssocMCSym->getName());
      continue;
    }

    const auto *AssocMCSec =
        static_cast<const MCSectionCOFF *>(&AssocMCSym->getSection());
    assert(SectionMap.count(AssocMCSec));
    COFFSection *AssocSec = SectionMap[AssocMCSec];

    // Skip this section if the associated section is unused.
    if (AssocSec->Number == -1)
      continue;

    Section->Symbol->Aux[0].Aux.SectionDefinition.Number = AssocSec->Number;
  }

  // Create the contents of the .llvm_addrsig section.
  if (Mode != DwoOnly && OWriter.getEmitAddrsigSection()) {
    SmallString<0> Content;
    raw_svector_ostream OS(Content);
    for (const MCSymbol *S : OWriter.AddrsigSyms) {
      if (!S->isRegistered())
        continue;
      if (!S->isTemporary()) {
        encodeULEB128(S->getIndex(), OS);
        continue;
      }

      MCSection *TargetSection = &S->getSection();
      assert(SectionMap.contains(TargetSection) &&
             "Section must already have been defined in "
             "executePostLayoutBinding!");
      encodeULEB128(SectionMap[TargetSection]->Symbol->getIndex(), OS);
    }
    auto *Sec = getContext().getCOFFSection(".llvm_addrsig",
                                            COFF::IMAGE_SCN_LNK_REMOVE);
    Sec->curFragList()->Tail->setVarContents(OS.str());
  }

  // Create the contents of the .gfids$y and .giats$y sections: the index of
  // each function, and of each import pointer, whose address a relocation of
  // the object takes. An undefined symbol without a type may be a function,
  // which the linker knows from its definition.
  if (Mode != DwoOnly &&
      getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
    std::set<uint32_t> Taken[2];
    for (const std::unique_ptr<COFFSection> &Section : Sections) {
      if (!COFF::isGuardCFAddressTakingSection(Section->Name))
        continue;
      // On ARM64 the load of an import pointer follows an adrp of its page,
      // which takes no address where the section's loads of the pointer are
      // fused reads.
      SmallPtrSet<const COFFSymbol *, 8> FusedRead;
      for (const COFFRelocation &R : Section->Relocations)
        if (R.Data.Type == COFF::IMAGE_REL_LLVM_FUSED_READ)
          FusedRead.insert(R.Symb);
      for (const COFFRelocation &R : Section->Relocations) {
        if (!COFF::isGuardCFAddressTakingRelocation(Header.Machine,
                                                    R.Data.Type) ||
            (Header.Machine == COFF::IMAGE_FILE_MACHINE_ARM64 &&
             R.Data.Type == COFF::IMAGE_REL_ARM64_PAGEBASE_REL21 &&
             FusedRead.contains(R.Symb)))
          continue;
        auto *Sym = static_cast<const MCSymbolCOFF *>(R.Symb->MC);
        bool IsImport = R.Symb->Name.starts_with("__imp_");
        if (IsImport ||
            (Sym && (Sym->getType()
                         ? Sym->getType() == COFF::IMAGE_SYM_DTYPE_FUNCTION
                                                 << COFF::SCT_COMPLEX_TYPE_SHIFT
                         : Sym->isUndefined())))
          Taken[IsImport].insert(R.Symb->getIndex());
      }
    }
    const MCObjectFileInfo *OFI = getContext().getObjectFileInfo();
    MCSection *Tables[] = {OFI->getGFIDsSection(), OFI->getGIATsSection()};
    for (unsigned I = 0; I != 2; ++I) {
      SmallString<0> Content;
      raw_svector_ostream OS(Content);
      for (uint32_t Index : Taken[I])
        support::endian::write(OS, Index, W.Endian);
      Tables[I]->curFragList()->Tail->setVarContents(OS.str());
    }
  }

  // Create the contents of the .llvm_link_records section. A group is its
  // kind, the size of its payload and the payload.
  if (Mode != DwoOnly &&
      getContext().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
    SmallString<0> Content;
    raw_svector_ostream OS(Content);
    OS.write(COFF::LinkRecordsMagic, sizeof(COFF::LinkRecordsMagic));
    encodeULEB128(COFF::LinkRecordsVersion, OS);
    encodeULEB128(OWriter.LinkRecordsFlags, OS);
    auto Group = [&](uint64_t Kind, StringRef Payload) {
      encodeULEB128(Kind, OS);
      encodeULEB128(Payload.size(), OS);
      OS << Payload;
    };

    // A fact names a symbol by its index in the symbol table, and a temporary
    // symbol by its section's, as .llvm_addrsig does. An operand that is not
    // a symbol is a number.
    SmallString<0> Facts;
    raw_svector_ostream FactsOS(Facts);
    auto Index = [&](const MCSymbol &S) {
      return S.isTemporary() ? SectionMap[&S.getSection()]->Symbol->getIndex()
                             : S.getIndex();
    };
    for (const WinCOFFObjectWriter::LinkFact &Fact : OWriter.LinkFacts) {
      SmallVector<uint64_t, 2> Operands;
      for (const MCExpr *Operand : Fact.Operands) {
        int64_t Value = 0;
        if (auto *Ref = dyn_cast<MCSymbolRefExpr>(Operand))
          Value = Index(Ref->getSymbol());
        else if (!Operand->evaluateAsAbsolute(Value, *Asm))
          getContext().reportError(
              Operand->getLoc(),
              "operand of a link fact is neither a symbol nor absolute");
        Operands.push_back(Value);
      }
      COFF::encodeLinkFact(FactsOS, Index(*Fact.Symbol), Fact.Tag, Operands);
    }
    Group(COFF::LinkRecordSymbols, Facts);

    // The Sections group counts and lists, by its symbol's index, each section
    // the linker may resize.
    SmallVector<uint32_t, 0> Resizable;
    for (const std::unique_ptr<COFFSection> &Section : Sections)
      if (Section->MCSection->isLinkerRelaxable())
        Resizable.push_back(Section->Symbol->getIndex());
    // The dispatches through jump tables follow, counted: for each the
    // labels of its branch, table and base, the count and the type of the
    // entries, the label of the load and the flags. Then, counted, each
    // section with loops that a cache line is to keep: the places within a
    // line that it is barred from plus sixteen times a count, and that many
    // loops, each its size less one plus thirty-two times the distance of
    // its offset from the last one's.
    using LinePlace = WinCOFFObjectWriter::LinePlace;
    SmallVector<std::pair<uint32_t, const LinePlace *>, 0> Placed;
    for (const std::unique_ptr<COFFSection> &Section : Sections) {
      auto It = OWriter.LinePlaces.find(Section->MCSection);
      if (It != OWriter.LinePlaces.end() &&
          (It->second.Places || !It->second.Loops.empty()))
        Placed.push_back({Section->Symbol->getIndex(), &It->second});
    }
    if (!Resizable.empty() || !SwitchLabels.empty() || !Placed.empty()) {
      SmallString<0> Payload;
      raw_svector_ostream PayloadOS(Payload);
      encodeULEB128(Resizable.size(), PayloadOS);
      for (uint32_t Index : Resizable)
        encodeULEB128(Index, PayloadOS);
      encodeULEB128(SwitchLabels.size() / 4, PayloadOS);
      COFFSymbol **Label = SwitchLabels.begin();
      for (const WinCOFFObjectWriter::SwitchTable &Table :
           OWriter.SwitchTables) {
        for (int I = 0; I != 3; ++I)
          encodeULEB128((*Label++)->getIndex(), PayloadOS);
        encodeULEB128(Table.Count, PayloadOS);
        encodeULEB128(Table.Type, PayloadOS);
        encodeULEB128((*Label++)->getIndex(), PayloadOS);
        encodeULEB128(Table.Flags, PayloadOS);
      }
      if (!Placed.empty())
        encodeULEB128(Placed.size(), PayloadOS);
      for (auto [Section, Place] : Placed) {
        encodeULEB128(Section, PayloadOS);
        encodeULEB128(Place->Loops.size() << 4 | Place->Places, PayloadOS);
        uint32_t Last = 0;
        for (auto [Offset, Size] : Place->Loops) {
          encodeULEB128((Offset - Last) << 5 | (Size - 1), PayloadOS);
          Last = Offset;
        }
      }
      Group(COFF::LinkRecordSections, Payload);
    }
    // The Spans group: each list is the symbols of its two sections, a count
    // and the spans, by their fields' offsets.
    SmallString<0> Payload;
    raw_svector_ostream PayloadOS(Payload);
    for (auto &[Key, List] : OWriter.Spans) {
      if (!Key.second->isLinkerRelaxable())
        continue;
      llvm::stable_sort(List, [](const auto &A, const auto &B) {
        return A.Offset < B.Offset;
      });
      for (const MCSection *S : {Key.first, Key.second})
        encodeULEB128(SectionMap[S]->Symbol->getIndex(), PayloadOS);
      encodeULEB128(List.size(), PayloadOS);
      uint32_t Last = 0;
      for (const WinCOFFObjectWriter::Span &S : List) {
        encodeULEB128(uint64_t(S.Offset - Last) << 4 | S.Kind, PayloadOS);
        Last = S.Offset;
        if (S.Kind == COFF::SpanAddend32)
          encodeSLEB128(S.Operand, PayloadOS);
        else if (S.Kind >= COFF::SpanData)
          encodeULEB128(S.Operand, PayloadOS);
      }
    }
    if (!Payload.empty())
      Group(COFF::LinkRecordSpans, Payload);
    // The SiteFacts group states each fact of the relocation that it is
    // of, by the relocation's index in its section's table, which is in the
    // order of the offsets where a site has facts.
    if (!OWriter.SiteFacts.empty()) {
      struct Entry {
        COFFSection *Section;
        uint32_t Relocation;
        uint64_t Fact, Value;
      };
      SmallVector<Entry, 0> Entries;
      // Markers at one offset are that offset's fused relocations in their
      // order, each with the facts of its own label.
      DenseMap<std::pair<const MCSection *, uint64_t>, unsigned> AtOffset;
      DenseMap<const MCSymbol *, unsigned> Nth;
      for (const MCSymbol *Marker : OWriter.FusedMarkers)
        Nth.try_emplace(
            Marker,
            AtOffset[{&Marker->getSection(), Asm->getSymbolOffset(*Marker)}]++);
      for (const WinCOFFObjectWriter::SiteFact &Fact : OWriter.SiteFacts) {
        COFFSection *Section = SectionMap.lookup(&Fact.At->getSection());
        uint64_t Offset = Asm->getSymbolOffset(*Fact.At);
        relocations Empty;
        relocations &Relocations = Section ? Section->Relocations : Empty;
        auto Site = partition_point(Relocations, [&](const COFFRelocation &R) {
          return R.Data.VirtualAddress < Offset;
        });
        if (auto It = Nth.find(Fact.At); It != Nth.end())
          for (auto [I, Skip] = std::pair(Site, It->second);
               I != Relocations.end() && I->Data.VirtualAddress == Offset; ++I)
            if (COFF::getFusedMarkerUse(Header.Machine, I->Data.Type) !=
                    COFF::FusedUse::None &&
                !Skip--) {
              Site = I;
              break;
            }
        if (Site == Relocations.end())
          getContext().reportError(SMLoc(), "site fact " + Twine(Fact.Fact) +
                                                " is of no relocation");
        else
          Entries.push_back({Section, uint32_t(Site - Relocations.begin()),
                             Fact.Fact, Fact.Value});
      }
      stable_sort(Entries, [](const Entry &A, const Entry &B) {
        return std::pair(A.Section->Number, A.Relocation) <
               std::pair(B.Section->Number, B.Relocation);
      });
      // Each section's part: the index of its symbol, a count, and for each
      // relocation with facts its index less that of the one before it, the
      // size of its facts and each fact with its value.
      SmallString<0> Payload;
      raw_svector_ostream PayloadOS(Payload);
      for (auto I = Entries.begin(), E = Entries.end(); I != E;) {
        COFFSection *Section = I->Section;
        SmallString<64> Sites;
        raw_svector_ostream SitesOS(Sites);
        uint64_t Count = 0;
        for (uint32_t Last = 0; I != E && I->Section == Section; ++Count) {
          SmallString<16> Bytes;
          raw_svector_ostream BytesOS(Bytes);
          uint32_t Relocation = I->Relocation;
          for (; I != E && I->Section == Section && I->Relocation == Relocation;
               ++I) {
            encodeULEB128(I->Fact, BytesOS);
            encodeULEB128(I->Value, BytesOS);
          }
          encodeULEB128(Relocation - Last, SitesOS);
          encodeULEB128(Bytes.size(), SitesOS);
          SitesOS << Bytes;
          Last = Relocation;
        }
        encodeULEB128(Section->Symbol->getIndex(), PayloadOS);
        encodeULEB128(Count, PayloadOS);
        PayloadOS << Sites;
      }
      Group(COFF::LinkRecordSiteFacts, Payload);
    }
    auto *Sec = getContext().getCOFFSection(".llvm_link_records",
                                            COFF::IMAGE_SCN_LNK_INFO);
    Sec->curFragList()->Tail->setVarContents(OS.str());
  }

  // Create the contents of the .llvm.call-graph-profile section.
  if (Mode != DwoOnly && !OWriter.getCGProfile().empty()) {
    SmallString<0> Content;
    raw_svector_ostream OS(Content);
    for (const auto &CGPE : OWriter.getCGProfile()) {
      uint32_t FromIndex = CGPE.From->getSymbol().getIndex();
      uint32_t ToIndex = CGPE.To->getSymbol().getIndex();
      support::endian::write(OS, FromIndex, W.Endian);
      support::endian::write(OS, ToIndex, W.Endian);
      support::endian::write(OS, CGPE.Count, W.Endian);
    }
    auto *Sec = getContext().getCOFFSection(".llvm.call-graph-profile",
                                            COFF::IMAGE_SCN_LNK_REMOVE);
    Sec->curFragList()->Tail->setVarContents(OS.str());
  }

  assignFileOffsets();

  // MS LINK expects to be able to use this timestamp to implement their
  // /INCREMENTAL feature.
  if (OWriter.IncrementalLinkerCompatible) {
    Header.TimeDateStamp = getTime();
  } else {
    // Have deterministic output if /INCREMENTAL isn't needed. Also matches GNU.
    Header.TimeDateStamp = 0;
  }

  // Write it all to disk...
  WriteFileHeader(Header);
  writeSectionHeaders();

#ifndef NDEBUG
  sections::iterator I = Sections.begin();
  sections::iterator IE = Sections.end();
  auto J = Asm->begin();
  auto JE = Asm->end();
  for (; I != IE && J != JE; ++I, ++J) {
    while (J != JE && ((Mode == NonDwoOnly && isDwoSection(*J)) ||
                       (Mode == DwoOnly && !isDwoSection(*J))))
      ++J;
    assert(J != JE && (**I).MCSection == &*J && "Wrong bound MCSection");
  }
#endif

  // Write section contents.
  for (std::unique_ptr<COFFSection> &Sec : Sections)
    writeSection(*Sec);

  assert(W.OS.tell() == Header.PointerToSymbolTable &&
         "Header::PointerToSymbolTable is insane!");

  // Write a symbol table.
  for (auto &Symbol : Symbols)
    if (Symbol->getIndex() != -1)
      WriteSymbol(*Symbol);

  // Write a string table, which completes the entire COFF file.
  Strings.write(W.OS);

  return W.OS.tell() - StartOffset;
}

int WinCOFFWriter::getSectionNumber(const MCSection &Section) const {
  return SectionMap.at(&Section)->Number;
}

//------------------------------------------------------------------------------
// WinCOFFObjectWriter class implementation

////////////////////////////////////////////////////////////////////////////////
// MCObjectWriter interface implementations

void WinCOFFObjectWriter::reset() {
  IncrementalLinkerCompatible = false;
  LinkFacts.clear();
  SiteFacts.clear();
  FusedMarkers.clear();
  FixedSizeSections.clear();
  SwitchTables.clear();
  Spans.clear();
  LinePlaces.clear();
  LinkRecordsFlags = 0;
  ObjWriter->reset();
  if (DwoWriter)
    DwoWriter->reset();
  MCObjectWriter::reset();
}

bool WinCOFFObjectWriter::statesSpans(const MCSection &Sec) const {
  return Sec.isLinkerRelaxable() && Spans.count({&Sec, &Sec});
}

void WinCOFFObjectWriter::addSpan(const MCFragment &F, const MCFixup &Fixup,
                                  const MCSection &Code, unsigned Kind,
                                  int64_t Operand) {
  Spans[{F.getParent(), &Code}].push_back(
      {uint32_t(Asm->getFragmentOffset(F) + Fixup.getOffset()), Kind, Operand});
}

void WinCOFFObjectWriter::setAssembler(MCAssembler *Asm) {
  MCObjectWriter::setAssembler(Asm);
  ObjWriter->setAssembler(Asm);
  if (DwoWriter)
    DwoWriter->setAssembler(Asm);
}

bool WinCOFFObjectWriter::isSymbolRefDifferenceFullyResolvedImpl(
    const MCSymbol &SymA, const MCFragment &FB, bool InSet,
    bool IsPCRel) const {
  // Don't drop relocations between functions, even if they are in the same text
  // section. Multiple Visual C++ linker features depend on having the
  // relocations present. The /INCREMENTAL flag will cause these relocations to
  // point to thunks, and the /GUARD:CF flag assumes that it can use relocations
  // to approximate the set of all address taken functions. LLD's implementation
  // of /GUARD:CF also relies on the existance of these relocations.
  uint16_t Type = static_cast<const MCSymbolCOFF &>(SymA).getType();
  if ((Type >> COFF::SCT_COMPLEX_TYPE_SHIFT) == COFF::IMAGE_SYM_DTYPE_FUNCTION)
    return false;
  return &SymA.getSection() == FB.getParent();
}

// The linker may resize a section with an instruction it may resize in which
// every value that depends on the layout is a relocation or CodeView. Padding
// before a branch without an ALIGN relocation is not, nor is alignment without
// one, an .org or a fill of computed length after the first such instruction,
// nor a section with a value that depends on its layout and no relocation. An
// AArch64 alignment of at most an instruction keeps its padding when the
// linker resizes code, and an x86-64 alignment pads nothing in any link where
// it is of one byte or names the end of its loop after layout, which a cache
// line keeps. Before layout (Laid false) all of this is known but whether an
// alignment of code and padding before a branch state their relocations.
bool WinCOFFObjectWriter::keepsSize(const MCSection &Sec, bool Laid) const {
  bool IsArm64 = COFF::isAnyArm64(TargetObjectWriter->getMachine());
  return Sec.isLinkerRelaxable() &&
         (FixedSizeSections.contains(&Sec) ||
          any_of(Sec, [&](const MCFragment &F) {
            int64_t Count;
            if (auto *BF = dyn_cast<MCBoundaryAlignFragment>(&F))
              return Laid && BF->getLastFragment() && !F.isLinkerRelaxable();
            if (F.getLayoutOrder() <= Sec.firstLinkerRelaxable())
              return false;
            return (F.getKind() == MCFragment::FT_Align &&
                    (Laid || !F.hasAlignEmitNops()) && !F.isLinkerRelaxable() &&
                    !(IsArm64 ? F.getAlignment() <= Align(4)
                              : F.getAlignment() == Align(1) ||
                                    F.getAlignLoopEnd())) ||
                   F.getKind() == MCFragment::FT_PrefAlign ||
                   F.getKind() == MCFragment::FT_Org ||
                   (F.getKind() == MCFragment::FT_Fill &&
                    !static_cast<const MCFillFragment &>(F)
                         .getNumValues()
                         .evaluateAsAbsolute(Count));
          }));
}

void WinCOFFObjectWriter::executePostLayoutBinding() {
  // A section the linker may not resize, which only its layout showed, is no
  // longer linker-relaxable, so that the assembler resolves what it can in it
  // and its padding is no ALIGN relocation. A loop that a cache line keeps
  // behind its first site was laid out for the linker to keep, which it then
  // cannot.
  for (MCSection &MCSec : *Asm) {
    if (!keepsSize(MCSec, true))
      continue;
    auto It = LinePlaces.find(&MCSec);
    if (It != LinePlaces.end() && It->second.Most)
      getContext().reportError(
          SMLoc(), "a line cannot keep a loop behind a resizable site in '" +
                       MCSec.getName() + "', which its layout leaves fixed");
    MCSec.setFirstLinkerRelaxable(-1u);
    for (MCFragment &F : MCSec)
      if (F.getKind() == MCFragment::FT_Align ||
          F.getKind() == MCFragment::FT_BoundaryAlign)
        F.clearVarFixups();
  }
  ObjWriter->executePostLayoutBinding();
  if (DwoWriter)
    DwoWriter->executePostLayoutBinding();
}

void WinCOFFObjectWriter::recordRelocation(const MCFragment &F,
                                           const MCFixup &Fixup, MCValue Target,
                                           uint64_t &FixedValue) {
  assert(!isDwoSection(*F.getParent()) && "No relocation in Dwo sections");
  ObjWriter->recordRelocation(F, Fixup, Target, FixedValue);
}

uint64_t WinCOFFObjectWriter::writeObject() {
  // If the assember had an error, then layout will not have completed, so we
  // cannot write an object file.
  if (getContext().hadError())
    return 0;

  uint64_t TotalSize = ObjWriter->writeObject();
  if (DwoWriter)
    TotalSize += DwoWriter->writeObject();
  return TotalSize;
}

int WinCOFFObjectWriter::getSectionNumber(const MCSection &Section) const {
  return ObjWriter->getSectionNumber(Section);
}

MCWinCOFFObjectTargetWriter::MCWinCOFFObjectTargetWriter(unsigned Machine_)
    : Machine(Machine_) {}

// Pin the vtable to this file.
void MCWinCOFFObjectTargetWriter::anchor() {}

//------------------------------------------------------------------------------
// WinCOFFObjectWriter factory function

std::unique_ptr<MCObjectWriter> llvm::createWinCOFFObjectWriter(
    std::unique_ptr<MCWinCOFFObjectTargetWriter> MOTW, raw_pwrite_stream &OS) {
  return std::make_unique<WinCOFFObjectWriter>(std::move(MOTW), OS);
}

std::unique_ptr<MCObjectWriter> llvm::createWinCOFFDwoObjectWriter(
    std::unique_ptr<MCWinCOFFObjectTargetWriter> MOTW, raw_pwrite_stream &OS,
    raw_pwrite_stream &DwoOS) {
  return std::make_unique<WinCOFFObjectWriter>(std::move(MOTW), OS, DwoOS);
}
