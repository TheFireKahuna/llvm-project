//===- llvm/BinaryFormat/COFF.cpp - The COFF format -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/raw_ostream.h"

// Maximum offsets for different string table entry encodings.
enum : unsigned { Max7DecimalOffset = 9999999U };
enum : uint64_t { MaxBase64Offset = 0xFFFFFFFFFULL }; // 64^6, including 0

// Encode a string table entry offset in base 64, padded to 6 chars, and
// prefixed with a double slash: '//AAAAAA', '//AAAAAB', ...
// Buffer must be at least 8 bytes large. No terminating null appended.
static void encodeBase64StringEntry(char *Buffer, uint64_t Value) {
  assert(Value > Max7DecimalOffset && Value <= MaxBase64Offset &&
         "Illegal section name encoding for value");

  static const char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "abcdefghijklmnopqrstuvwxyz"
                                 "0123456789+/";

  Buffer[0] = '/';
  Buffer[1] = '/';

  char *Ptr = Buffer + 7;
  for (unsigned i = 0; i < 6; ++i) {
    unsigned Rem = Value % 64;
    Value /= 64;
    *(Ptr--) = Alphabet[Rem];
  }
}

bool llvm::COFF::encodeSectionName(char *Out, uint64_t Offset) {
  if (Offset <= Max7DecimalOffset) {
    // Offsets of 7 digits or less are encoded in ASCII.
    SmallVector<char, COFF::NameSize> Buffer;
    Twine('/').concat(Twine(Offset)).toVector(Buffer);
    assert(Buffer.size() <= COFF::NameSize && Buffer.size() >= 2);
    std::memcpy(Out, Buffer.data(), Buffer.size());
    return true;
  }

  if (Offset <= MaxBase64Offset) {
    // Starting with 10,000,000, offsets are encoded as base64.
    encodeBase64StringEntry(Out, Offset);
    return true;
  }

  // The offset is too large to be encoded.
  return false;
}

bool llvm::COFF::isVagueEntityName(StringRef Name) {
  if (Name.starts_with("?") || Name.starts_with("."))
    return false;
  if (!Name.consume_front("_Z"))
    return true;
  return !is_contained({"TV", "TT", "TC", "TS", "GV", "TH", "TW"},
                       Name.take_front(2));
}

void llvm::COFF::encodeLinkFact(raw_ostream &OS, uint64_t Symbol, uint64_t Tag,
                                ArrayRef<uint64_t> Operands) {
  SmallString<16> Bytes;
  raw_svector_ostream BytesOS(Bytes);
  for (uint64_t Operand : Operands)
    encodeULEB128(Operand, BytesOS);
  encodeULEB128(Symbol, OS);
  encodeULEB128(Tag, OS);
  encodeULEB128(Bytes.size(), OS);
  OS << Bytes;
}

bool llvm::COFF::isGuardCFAddressTakingSection(StringRef SectionName) {
  return SectionName.split('$').first != ".pdata" &&
         !SectionName.starts_with(".debug") &&
         !SectionName.ends_with("_$fo_rvas$");
}

bool llvm::COFF::isGuardCFAddressTakingRelocation(uint16_t Machine,
                                                  uint16_t Type) {
  if (relocationNamesNoSymbol(Type))
    return false;
  switch (Machine) {
  case IMAGE_FILE_MACHINE_I386:
    return Type != IMAGE_REL_I386_REL32;
  case IMAGE_FILE_MACHINE_AMD64:
    if (Type == IMAGE_REL_AMD64_PAIR || Type == IMAGE_REL_AMD64_SSPAN32)
      return false;
    break;
  case IMAGE_FILE_MACHINE_ARM64:
    if (Type == IMAGE_REL_ARM64_PAIR || Type == IMAGE_REL_ARM64_BRANCH26 ||
        Type == IMAGE_REL_ARM64_BRANCH19 || Type == IMAGE_REL_ARM64_BRANCH14)
      return false;
    break;
  default:
    return true;
  }
  // An LLVM type states whether it does.
  if (uint8_t Props = getLLVMRelocProperties(Type))
    return Props & LLVMReloc::Address;
  return true;
}

llvm::StringRef llvm::COFF::getLLVMRelocName(uint16_t Machine, uint16_t Type) {
  if (!(getLLVMRelocProperties(Type) & getLLVMRelocMachine(Machine)))
    return {};
  switch (Type) {
#define COFF_LLVM_RELOC(Name, Value, Props, Use)                               \
  case Value:                                                                  \
    return "IMAGE_REL_LLVM_" #Name;
#include "llvm/BinaryFormat/COFFRelocs/LLVM.def"
#undef COFF_LLVM_RELOC
  }
  return isRel32Insn(Type) ? "IMAGE_REL_LLVM_REL32_INSN" : StringRef();
}

uint16_t llvm::COFF::getLLVMRelocType(uint16_t Machine, StringRef Name) {
  uint16_t Type = StringSwitch<uint16_t>(Name)
#define COFF_LLVM_RELOC(Name, Value, Props, Use)                               \
  .Case("IMAGE_REL_LLVM_" #Name, Value)
#include "llvm/BinaryFormat/COFFRelocs/LLVM.def"
#undef COFF_LLVM_RELOC
                      .Default(0);
  return getLLVMRelocProperties(Type) & getLLVMRelocMachine(Machine) ? Type : 0;
}

uint16_t llvm::COFF::getFusedMarker(uint16_t Machine, FusedUse Use) {
  static constexpr uint16_t Types[] = {
#define COFF_LLVM_RELOC(Name, Value, Props, Use) Value,
#include "llvm/BinaryFormat/COFFRelocs/LLVM.def"
#undef COFF_LLVM_RELOC
  };
  for (uint16_t Type : Types)
    if (getFusedMarkerUse(Machine, Type) == Use)
      return Type;
  return 0;
}
