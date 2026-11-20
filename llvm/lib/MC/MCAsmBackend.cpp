//===- MCAsmBackend.cpp - Target MC Assembly Backend ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCAsmBackend.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDXContainerWriter.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCGOFFObjectWriter.h"
#include "llvm/MC/MCMachObjectWriter.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSPIRVObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCWasmObjectWriter.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/MC/MCXCOFFObjectWriter.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace llvm;

MCAsmBackend::~MCAsmBackend() = default;

MCContext &MCAsmBackend::getContext() const { return Asm->getContext(); }

std::unique_ptr<MCObjectWriter>
MCAsmBackend::createObjectWriter(raw_pwrite_stream &OS) const {
  auto TW = createObjectTargetWriter();
  bool IsLE = Endian == llvm::endianness::little;
  switch (TW->getFormat()) {
  case Triple::MachO:
    return std::make_unique<MachObjectWriter>(
        cast<MCMachObjectTargetWriter>(std::move(TW)), OS, IsLE);
  case Triple::COFF:
    return createWinCOFFObjectWriter(
        cast<MCWinCOFFObjectTargetWriter>(std::move(TW)), OS);
  case Triple::ELF:
    return std::make_unique<ELFObjectWriter>(
        cast<MCELFObjectTargetWriter>(std::move(TW)), OS, IsLE);
  case Triple::SPIRV:
    return createSPIRVObjectWriter(
        cast<MCSPIRVObjectTargetWriter>(std::move(TW)), OS);
  case Triple::Wasm:
    return createWasmObjectWriter(cast<MCWasmObjectTargetWriter>(std::move(TW)),
                                  OS);
  case Triple::GOFF:
    return createGOFFObjectWriter(cast<MCGOFFObjectTargetWriter>(std::move(TW)),
                                  OS);
  case Triple::XCOFF:
    return createXCOFFObjectWriter(
        cast<MCXCOFFObjectTargetWriter>(std::move(TW)), OS);
  case Triple::DXContainer:
    return std::make_unique<DXContainerObjectWriter>(
        cast<MCDXContainerTargetWriter>(std::move(TW)), OS);
  default:
    llvm_unreachable("unexpected object format");
  }
}

std::unique_ptr<MCObjectWriter>
MCAsmBackend::createDwoObjectWriter(raw_pwrite_stream &OS,
                                    raw_pwrite_stream &DwoOS) const {
  auto TW = createObjectTargetWriter();
  switch (TW->getFormat()) {
  case Triple::COFF:
    return createWinCOFFDwoObjectWriter(
        cast<MCWinCOFFObjectTargetWriter>(std::move(TW)), OS, DwoOS);
  case Triple::ELF:
    return std::make_unique<ELFObjectWriter>(
        cast<MCELFObjectTargetWriter>(std::move(TW)), OS, DwoOS,
        Endian == llvm::endianness::little);
  case Triple::GOFF:
    return createGOFFObjectWriter(cast<MCGOFFObjectTargetWriter>(std::move(TW)),
                                  OS, DwoOS);
  case Triple::Wasm:
    return createWasmDwoObjectWriter(
        cast<MCWasmObjectTargetWriter>(std::move(TW)), OS, DwoOS);
  default:
    report_fatal_error("dwo only supported with COFF, ELF, GOFF, and Wasm");
  }
}

std::optional<MCFixupKind> MCAsmBackend::getFixupKind(StringRef Name) const {
  return std::nullopt;
}

MCFixupKindInfo MCAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  // clang-format off
  static const MCFixupKindInfo Builtins[] = {
      {"FK_NONE", 0, 0, 0},
      {"FK_Data_1", 0, 8, 0},
      {"FK_Data_2", 0, 16, 0},
      {"FK_Data_4", 0, 32, 0},
      {"FK_Data_8", 0, 64, 0},
      {"FK_Data_leb128", 0, 0, 0},
      {"FK_SecRel_1", 0, 8, 0},
      {"FK_SecRel_2", 0, 16, 0},
      {"FK_SecRel_4", 0, 32, 0},
      {"FK_SecRel_8", 0, 64, 0},
  };
  // clang-format on

  assert(size_t(Kind - FK_NONE) < std::size(Builtins) && "Unknown fixup kind");
  return Builtins[Kind - FK_NONE];
}

bool MCAsmBackend::fixupNeedsRelaxationAdvanced(const MCFragment &,
                                                const MCFixup &,
                                                const MCValue &, uint64_t,
                                                bool Resolved) const {
  return !Resolved;
}

void MCAsmBackend::maybeAddReloc(const MCFragment &F, const MCFixup &Fixup,
                                 const MCValue &Target, uint64_t &Value,
                                 bool IsResolved) {
  if (!IsResolved)
    Asm->getWriter().recordRelocation(F, Fixup, Target, Value);
}

bool MCAsmBackend::isPCRelFixupResolved(const MCSymbol *SymA,
                                        const MCFragment &F) {
  // If the section does not contain linker-relaxable fragments, PC-relative
  // fixups can be resolved.
  if (!F.getParent()->isLinkerRelaxable())
    return true;

  // Otherwise, check if the offset between the symbol and fragment is fully
  // resolved, unaffected by linker-relaxable fragments (e.g. instructions or
  // offset-affected FT_Align fragments). Complements the generic
  // isSymbolRefDifferenceFullyResolvedImpl.
  if (!PCRelTemp)
    PCRelTemp = getContext().createTempSymbol();
  PCRelTemp->setFragment(const_cast<MCFragment *>(&F));
  MCValue Res;
  MCExpr::evaluateSymbolicAdd(Asm, false, MCValue::get(SymA),
                              MCValue::get(nullptr, PCRelTemp), Res);
  return !Res.getSubSym();
}

bool MCAsmBackend::relaxDwarfLineAddrWithFixup(MCFragment &F) const {
  int64_t LineDelta = F.getDwarfLineDelta();
  const MCExpr &AddrDelta = F.getDwarfAddrDelta();
  int64_t Value;
  // If the label difference can be resolved, use the default handling, which
  // utilizes a shorter special opcode.
  if (AddrDelta.evaluateAsAbsolute(Value, *Asm))
    return false;
  [[maybe_unused]] bool IsAbsolute =
      AddrDelta.evaluateKnownAbsolute(Value, *Asm);
  assert(IsAbsolute && "CFA with invalid expression");

  SmallVector<char> Data;
  raw_svector_ostream OS(Data);

  // INT64_MAX is a signal that this is actually a DW_LNE_end_sequence.
  if (LineDelta != INT64_MAX) {
    OS << uint8_t(dwarf::DW_LNS_advance_line);
    encodeSLEB128(LineDelta, OS);
  }

  // According to the DWARF specification, the `DW_LNS_fixed_advance_pc` opcode
  // takes a single unsigned half (unencoded) operand. The maximum encodable
  // value is therefore 65535.  Set a conservative upper bound for relaxation.
  unsigned PCBytes;
  if (Value > 60000) {
    PCBytes = getContext().getAsmInfo().getCodePointerSize();
    assert((PCBytes == 4 || PCBytes == 8) && "Unexpected pointer size");
    OS << uint8_t(dwarf::DW_LNS_extended_op) << uint8_t(PCBytes + 1)
       << uint8_t(dwarf::DW_LNE_set_address);
  } else {
    PCBytes = 2;
    OS << uint8_t(dwarf::DW_LNS_fixed_advance_pc);
  }
  auto Offset = OS.tell();
  OS.write_zeros(PCBytes);

  if (LineDelta == INT64_MAX) {
    OS << uint8_t(dwarf::DW_LNS_extended_op);
    OS << uint8_t(1);
    OS << uint8_t(dwarf::DW_LNE_end_sequence);
  } else {
    OS << uint8_t(dwarf::DW_LNS_copy);
  }

  F.setVarContents(Data);
  F.setVarFixups({MCFixup::create(Offset, &AddrDelta,
                                  MCFixup::getDataKindForSize(PCBytes))});
  return true;
}

bool MCAsmBackend::relaxDwarfCFAWithFixups(
    MCFragment &F, unsigned MinBits,
    function_ref<void(unsigned Bits, unsigned Offset,
                      SmallVectorImpl<MCFixup> &Fixups)>
        AddFixups) const {
  const MCExpr &AddrDelta = F.getDwarfAddrDelta();
  int64_t Value;
  if (AddrDelta.evaluateAsAbsolute(Value, *Asm))
    return false;
  [[maybe_unused]] bool IsAbsolute =
      AddrDelta.evaluateKnownAbsolute(Value, *Asm);
  assert(IsAbsolute && "CFA with invalid expression");

  assert(getContext().getAsmInfo().getMinInstAlignment() == 1 &&
         "expected 1-byte alignment");
  if (Value == 0) {
    F.clearVarContents();
    F.clearVarFixups();
    return true;
  }

  // DW_CFA_advance_loc holds the delta in the low 6 bits of its opcode; the
  // other forms take an operand of 1, 2 or 4 bytes.
  SmallVector<char, 8> Data;
  raw_svector_ostream OS(Data);
  unsigned Bits;
  if (MinBits <= 6 && isUIntN(6, Value)) {
    Bits = 6;
    OS << uint8_t(dwarf::DW_CFA_advance_loc);
  } else if (isUInt<8>(Value)) {
    Bits = 8;
    OS << uint8_t(dwarf::DW_CFA_advance_loc1);
  } else if (isUInt<16>(Value)) {
    Bits = 16;
    OS << uint8_t(dwarf::DW_CFA_advance_loc2);
  } else if (isUInt<32>(Value)) {
    Bits = 32;
    OS << uint8_t(dwarf::DW_CFA_advance_loc4);
  } else {
    llvm_unreachable("unsupported CFA encoding");
  }
  OS.write_zeros(Bits / 8);
  SmallVector<MCFixup, 2> Fixups;
  AddFixups(Bits, Bits != 6, Fixups);
  F.setVarContents(Data);
  F.setVarFixups(Fixups);
  return true;
}

bool MCAsmBackend::isDarwinCanonicalPersonality(const MCSymbol *Sym) const {
  assert(getContext().isMachO());
  // Consider a NULL personality (ie., no personality encoding) to be canonical
  // because it's always at 0.
  if (!Sym)
    return true;

  StringRef name = Sym->getName();
  // XXX: We intentionally leave out "___gcc_personality_v0" because, despite
  // being system-defined like these two, it is not very commonly-used.
  // Reserving an empty slot for it seems silly.
  return name == "___gxx_personality_v0" || name == "___objc_personality_v0";
}

const MCSubtargetInfo *MCAsmBackend::getSubtargetInfo(const MCFragment &F) {
  const MCSubtargetInfo *STI = nullptr;
  STI = F.getSubtargetInfo();
  assert(!F.hasInstructions() || STI != nullptr);
  return STI;
}
