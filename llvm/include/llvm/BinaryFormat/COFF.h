//===-- llvm/BinaryFormat/COFF.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains an definitions used in Windows COFF Files.
//
// Structures and enums defined within this file where created using
// information from Microsoft's publicly available PE/COFF format document:
//
// Microsoft Portable Executable and Common Object File Format Specification
// Revision 8.1 - February 15, 2008
//
// As of 5/2/2010, hosted by Microsoft at:
// http://www.microsoft.com/whdc/system/platform/firmware/pecoff.mspx
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BINARYFORMAT_COFF_H
#define LLVM_BINARYFORMAT_COFF_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/DataTypes.h"
#include <cassert>

namespace llvm {
class raw_ostream;

namespace COFF {

// The maximum number of sections that a COFF object can have (inclusive).
const int32_t MaxNumberOfSections16 = 65279;

// The PE signature bytes that follows the DOS stub header.
static const char PEMagic[] = {'P', 'E', '\0', '\0'};

static const char BigObjMagic[] = {
    '\xc7', '\xa1', '\xba', '\xd1', '\xee', '\xba', '\xa9', '\x4b',
    '\xaf', '\x20', '\xfa', '\xf6', '\x6a', '\xa4', '\xdc', '\xb8',
};

static const char ClGlObjMagic[] = {
    '\x38', '\xfe', '\xb3', '\x0c', '\xa5', '\xd9', '\xab', '\x4d',
    '\xac', '\x9b', '\xd6', '\xb6', '\x22', '\x26', '\x53', '\xc2',
};

// The signature bytes that start a .res file.
static const char WinResMagic[] = {
    '\x00', '\x00', '\x00', '\x00', '\x20', '\x00', '\x00', '\x00',
    '\xff', '\xff', '\x00', '\x00', '\xff', '\xff', '\x00', '\x00',
};

// Sizes in bytes of various things in the COFF format.
enum {
  Header16Size = 20,
  Header32Size = 56,
  NameSize = 8,
  Symbol16Size = 18,
  Symbol32Size = 20,
  SectionSize = 40,
  RelocationSize = 10
};

struct header {
  uint16_t Machine;
  int32_t NumberOfSections;
  uint32_t TimeDateStamp;
  uint32_t PointerToSymbolTable;
  uint32_t NumberOfSymbols;
  uint16_t SizeOfOptionalHeader;
  uint16_t Characteristics;
};

struct BigObjHeader {
  enum : uint16_t { MinBigObjectVersion = 2 };

  uint16_t Sig1; ///< Must be IMAGE_FILE_MACHINE_UNKNOWN (0).
  uint16_t Sig2; ///< Must be 0xFFFF.
  uint16_t Version;
  uint16_t Machine;
  uint32_t TimeDateStamp;
  uint8_t UUID[16];
  uint32_t unused1;
  uint32_t unused2;
  uint32_t unused3;
  uint32_t unused4;
  uint32_t NumberOfSections;
  uint32_t PointerToSymbolTable;
  uint32_t NumberOfSymbols;
};

enum MachineTypes : unsigned {
  MT_Invalid = 0xffff,

  IMAGE_FILE_MACHINE_UNKNOWN = 0x0,
  IMAGE_FILE_MACHINE_AM33 = 0x1D3,
  IMAGE_FILE_MACHINE_AMD64 = 0x8664,
  IMAGE_FILE_MACHINE_ARM = 0x1C0,
  IMAGE_FILE_MACHINE_ARMNT = 0x1C4,
  IMAGE_FILE_MACHINE_ARM64 = 0xAA64,
  IMAGE_FILE_MACHINE_ARM64EC = 0xA641,
  IMAGE_FILE_MACHINE_ARM64X = 0xA64E,
  IMAGE_FILE_MACHINE_EBC = 0xEBC,
  IMAGE_FILE_MACHINE_I386 = 0x14C,
  IMAGE_FILE_MACHINE_IA64 = 0x200,
  IMAGE_FILE_MACHINE_M32R = 0x9041,
  IMAGE_FILE_MACHINE_MIPS16 = 0x266,
  IMAGE_FILE_MACHINE_MIPSFPU = 0x366,
  IMAGE_FILE_MACHINE_MIPSFPU16 = 0x466,
  IMAGE_FILE_MACHINE_POWERPC = 0x1F0,
  IMAGE_FILE_MACHINE_POWERPCFP = 0x1F1,
  IMAGE_FILE_MACHINE_R4000 = 0x166,
  IMAGE_FILE_MACHINE_RISCV32 = 0x5032,
  IMAGE_FILE_MACHINE_RISCV64 = 0x5064,
  IMAGE_FILE_MACHINE_RISCV128 = 0x5128,
  IMAGE_FILE_MACHINE_SH3 = 0x1A2,
  IMAGE_FILE_MACHINE_SH3DSP = 0x1A3,
  IMAGE_FILE_MACHINE_SH4 = 0x1A6,
  IMAGE_FILE_MACHINE_SH5 = 0x1A8,
  IMAGE_FILE_MACHINE_THUMB = 0x1C2,
  IMAGE_FILE_MACHINE_WCEMIPSV2 = 0x169
};

template <typename T> bool isArm64EC(T Machine) {
  return Machine == IMAGE_FILE_MACHINE_ARM64EC ||
         Machine == IMAGE_FILE_MACHINE_ARM64X;
}

template <typename T> bool isAnyArm64(T Machine) {
  return Machine == IMAGE_FILE_MACHINE_ARM64 || isArm64EC(Machine);
}

template <typename T> bool is64Bit(T Machine) {
  return Machine == IMAGE_FILE_MACHINE_AMD64 || isAnyArm64(Machine);
}

enum Characteristics : unsigned {
  C_Invalid = 0,

  /// The file does not contain base relocations and must be loaded at its
  /// preferred base. If this cannot be done, the loader will error.
  IMAGE_FILE_RELOCS_STRIPPED = 0x0001,
  /// The file is valid and can be run.
  IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002,
  /// COFF line numbers have been stripped. This is deprecated and should be
  /// 0.
  IMAGE_FILE_LINE_NUMS_STRIPPED = 0x0004,
  /// COFF symbol table entries for local symbols have been removed. This is
  /// deprecated and should be 0.
  IMAGE_FILE_LOCAL_SYMS_STRIPPED = 0x0008,
  /// Aggressively trim working set. This is deprecated and must be 0.
  IMAGE_FILE_AGGRESSIVE_WS_TRIM = 0x0010,
  /// Image can handle > 2GiB addresses.
  IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x0020,
  /// Little endian: the LSB precedes the MSB in memory. This is deprecated
  /// and should be 0.
  IMAGE_FILE_BYTES_REVERSED_LO = 0x0080,
  /// Machine is based on a 32bit word architecture.
  IMAGE_FILE_32BIT_MACHINE = 0x0100,
  /// Debugging info has been removed.
  IMAGE_FILE_DEBUG_STRIPPED = 0x0200,
  /// If the image is on removable media, fully load it and copy it to swap.
  IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP = 0x0400,
  /// If the image is on network media, fully load it and copy it to swap.
  IMAGE_FILE_NET_RUN_FROM_SWAP = 0x0800,
  /// The image file is a system file, not a user program.
  IMAGE_FILE_SYSTEM = 0x1000,
  /// The image file is a DLL.
  IMAGE_FILE_DLL = 0x2000,
  /// This file should only be run on a uniprocessor machine.
  IMAGE_FILE_UP_SYSTEM_ONLY = 0x4000,
  /// Big endian: the MSB precedes the LSB in memory. This is deprecated
  /// and should be 0.
  IMAGE_FILE_BYTES_REVERSED_HI = 0x8000
};

enum ResourceTypeID : unsigned {
  RID_Cursor = 1,
  RID_Bitmap = 2,
  RID_Icon = 3,
  RID_Menu = 4,
  RID_Dialog = 5,
  RID_String = 6,
  RID_FontDir = 7,
  RID_Font = 8,
  RID_Accelerator = 9,
  RID_RCData = 10,
  RID_MessageTable = 11,
  RID_Group_Cursor = 12,
  RID_Group_Icon = 14,
  RID_Version = 16,
  RID_DLGInclude = 17,
  RID_PlugPlay = 19,
  RID_VXD = 20,
  RID_AniCursor = 21,
  RID_AniIcon = 22,
  RID_HTML = 23,
  RID_Manifest = 24,
};

struct symbol {
  char Name[NameSize];
  uint32_t Value;
  int32_t SectionNumber;
  uint16_t Type;
  uint8_t StorageClass;
  uint8_t NumberOfAuxSymbols;
};

enum SymbolSectionNumber : int32_t {
  IMAGE_SYM_DEBUG = -2,
  IMAGE_SYM_ABSOLUTE = -1,
  IMAGE_SYM_UNDEFINED = 0
};

/// Storage class tells where and what the symbol represents
enum SymbolStorageClass {
  SSC_Invalid = 0xff,

  IMAGE_SYM_CLASS_END_OF_FUNCTION = -1,  ///< Physical end of function
  IMAGE_SYM_CLASS_NULL = 0,              ///< No symbol
  IMAGE_SYM_CLASS_AUTOMATIC = 1,         ///< Stack variable
  IMAGE_SYM_CLASS_EXTERNAL = 2,          ///< External symbol
  IMAGE_SYM_CLASS_STATIC = 3,            ///< Static
  IMAGE_SYM_CLASS_REGISTER = 4,          ///< Register variable
  IMAGE_SYM_CLASS_EXTERNAL_DEF = 5,      ///< External definition
  IMAGE_SYM_CLASS_LABEL = 6,             ///< Label
  IMAGE_SYM_CLASS_UNDEFINED_LABEL = 7,   ///< Undefined label
  IMAGE_SYM_CLASS_MEMBER_OF_STRUCT = 8,  ///< Member of structure
  IMAGE_SYM_CLASS_ARGUMENT = 9,          ///< Function argument
  IMAGE_SYM_CLASS_STRUCT_TAG = 10,       ///< Structure tag
  IMAGE_SYM_CLASS_MEMBER_OF_UNION = 11,  ///< Member of union
  IMAGE_SYM_CLASS_UNION_TAG = 12,        ///< Union tag
  IMAGE_SYM_CLASS_TYPE_DEFINITION = 13,  ///< Type definition
  IMAGE_SYM_CLASS_UNDEFINED_STATIC = 14, ///< Undefined static
  IMAGE_SYM_CLASS_ENUM_TAG = 15,         ///< Enumeration tag
  IMAGE_SYM_CLASS_MEMBER_OF_ENUM = 16,   ///< Member of enumeration
  IMAGE_SYM_CLASS_REGISTER_PARAM = 17,   ///< Register parameter
  IMAGE_SYM_CLASS_BIT_FIELD = 18,        ///< Bit field
  /// ".bb" or ".eb" - beginning or end of block
  IMAGE_SYM_CLASS_BLOCK = 100,
  /// ".bf" or ".ef" - beginning or end of function
  IMAGE_SYM_CLASS_FUNCTION = 101,
  IMAGE_SYM_CLASS_END_OF_STRUCT = 102, ///< End of structure
  IMAGE_SYM_CLASS_FILE = 103,          ///< File name
  /// Line number, reformatted as symbol
  IMAGE_SYM_CLASS_SECTION = 104,
  IMAGE_SYM_CLASS_WEAK_EXTERNAL = 105, ///< Duplicate tag
  /// External symbol in dmert public lib
  IMAGE_SYM_CLASS_CLR_TOKEN = 107
};

enum SymbolBaseType : unsigned {
  IMAGE_SYM_TYPE_NULL = 0,   ///< No type information or unknown base type.
  IMAGE_SYM_TYPE_VOID = 1,   ///< Used with void pointers and functions.
  IMAGE_SYM_TYPE_CHAR = 2,   ///< A character (signed byte).
  IMAGE_SYM_TYPE_SHORT = 3,  ///< A 2-byte signed integer.
  IMAGE_SYM_TYPE_INT = 4,    ///< A natural integer type on the target.
  IMAGE_SYM_TYPE_LONG = 5,   ///< A 4-byte signed integer.
  IMAGE_SYM_TYPE_FLOAT = 6,  ///< A 4-byte floating-point number.
  IMAGE_SYM_TYPE_DOUBLE = 7, ///< An 8-byte floating-point number.
  IMAGE_SYM_TYPE_STRUCT = 8, ///< A structure.
  IMAGE_SYM_TYPE_UNION = 9,  ///< An union.
  IMAGE_SYM_TYPE_ENUM = 10,  ///< An enumerated type.
  IMAGE_SYM_TYPE_MOE = 11,   ///< A member of enumeration (a specific value).
  IMAGE_SYM_TYPE_BYTE = 12,  ///< A byte; unsigned 1-byte integer.
  IMAGE_SYM_TYPE_WORD = 13,  ///< A word; unsigned 2-byte integer.
  IMAGE_SYM_TYPE_UINT = 14,  ///< An unsigned integer of natural size.
  IMAGE_SYM_TYPE_DWORD = 15  ///< An unsigned 4-byte integer.
};

enum SymbolComplexType : unsigned {
  IMAGE_SYM_DTYPE_NULL = 0,     ///< No complex type; simple scalar variable.
  IMAGE_SYM_DTYPE_POINTER = 1,  ///< A pointer to base type.
  IMAGE_SYM_DTYPE_FUNCTION = 2, ///< A function that returns a base type.
  IMAGE_SYM_DTYPE_ARRAY = 3,    ///< An array of base type.

  /// Type is formed as (base + (derived << SCT_COMPLEX_TYPE_SHIFT))
  SCT_COMPLEX_TYPE_SHIFT = 4
};

enum AuxSymbolType { IMAGE_AUX_SYMBOL_TYPE_TOKEN_DEF = 1 };

struct section {
  char Name[NameSize];
  uint32_t VirtualSize;
  uint32_t VirtualAddress;
  uint32_t SizeOfRawData;
  uint32_t PointerToRawData;
  uint32_t PointerToRelocations;
  uint32_t PointerToLineNumbers;
  uint16_t NumberOfRelocations;
  uint16_t NumberOfLineNumbers;
  uint32_t Characteristics;
};

enum SectionCharacteristics : uint32_t {
  SC_Invalid = 0xffffffff,

  IMAGE_SCN_TYPE_NOLOAD = 0x00000002,
  IMAGE_SCN_TYPE_NO_PAD = 0x00000008,
  IMAGE_SCN_CNT_CODE = 0x00000020,
  IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040,
  IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080,
  IMAGE_SCN_LNK_OTHER = 0x00000100,
  IMAGE_SCN_LNK_INFO = 0x00000200,
  IMAGE_SCN_LNK_REMOVE = 0x00000800,
  IMAGE_SCN_LNK_COMDAT = 0x00001000,
  IMAGE_SCN_GPREL = 0x00008000,
  IMAGE_SCN_MEM_PURGEABLE = 0x00020000,
  IMAGE_SCN_MEM_16BIT = 0x00020000,
  IMAGE_SCN_MEM_LOCKED = 0x00040000,
  IMAGE_SCN_MEM_PRELOAD = 0x00080000,
  IMAGE_SCN_ALIGN_1BYTES = 0x00100000,
  IMAGE_SCN_ALIGN_2BYTES = 0x00200000,
  IMAGE_SCN_ALIGN_4BYTES = 0x00300000,
  IMAGE_SCN_ALIGN_8BYTES = 0x00400000,
  IMAGE_SCN_ALIGN_16BYTES = 0x00500000,
  IMAGE_SCN_ALIGN_32BYTES = 0x00600000,
  IMAGE_SCN_ALIGN_64BYTES = 0x00700000,
  IMAGE_SCN_ALIGN_128BYTES = 0x00800000,
  IMAGE_SCN_ALIGN_256BYTES = 0x00900000,
  IMAGE_SCN_ALIGN_512BYTES = 0x00A00000,
  IMAGE_SCN_ALIGN_1024BYTES = 0x00B00000,
  IMAGE_SCN_ALIGN_2048BYTES = 0x00C00000,
  IMAGE_SCN_ALIGN_4096BYTES = 0x00D00000,
  IMAGE_SCN_ALIGN_8192BYTES = 0x00E00000,
  IMAGE_SCN_ALIGN_MASK = 0x00F00000,
  IMAGE_SCN_LNK_NRELOC_OVFL = 0x01000000,
  IMAGE_SCN_MEM_DISCARDABLE = 0x02000000,
  IMAGE_SCN_MEM_NOT_CACHED = 0x04000000,
  IMAGE_SCN_MEM_NOT_PAGED = 0x08000000,
  IMAGE_SCN_MEM_SHARED = 0x10000000,
  IMAGE_SCN_MEM_EXECUTE = 0x20000000,
  IMAGE_SCN_MEM_READ = 0x40000000,
  IMAGE_SCN_MEM_WRITE = 0x80000000
};

struct relocation {
  uint32_t VirtualAddress;
  uint32_t SymbolTableIndex;
  uint16_t Type;
};

enum RelocationTypeI386 : unsigned {
  IMAGE_REL_I386_ABSOLUTE = 0x0000,
  IMAGE_REL_I386_DIR16 = 0x0001,
  IMAGE_REL_I386_REL16 = 0x0002,
  IMAGE_REL_I386_DIR32 = 0x0006,
  IMAGE_REL_I386_DIR32NB = 0x0007,
  IMAGE_REL_I386_SEG12 = 0x0009,
  IMAGE_REL_I386_SECTION = 0x000A,
  IMAGE_REL_I386_SECREL = 0x000B,
  IMAGE_REL_I386_TOKEN = 0x000C,
  IMAGE_REL_I386_SECREL7 = 0x000D,
  IMAGE_REL_I386_REL32 = 0x0014
};

enum RelocationTypeAMD64 : unsigned {
  IMAGE_REL_AMD64_ABSOLUTE = 0x0000,
  IMAGE_REL_AMD64_ADDR64 = 0x0001,
  IMAGE_REL_AMD64_ADDR32 = 0x0002,
  IMAGE_REL_AMD64_ADDR32NB = 0x0003,
  IMAGE_REL_AMD64_REL32 = 0x0004,
  IMAGE_REL_AMD64_REL32_1 = 0x0005,
  IMAGE_REL_AMD64_REL32_2 = 0x0006,
  IMAGE_REL_AMD64_REL32_3 = 0x0007,
  IMAGE_REL_AMD64_REL32_4 = 0x0008,
  IMAGE_REL_AMD64_REL32_5 = 0x0009,
  IMAGE_REL_AMD64_SECTION = 0x000A,
  IMAGE_REL_AMD64_SECREL = 0x000B,
  IMAGE_REL_AMD64_SECREL7 = 0x000C,
  IMAGE_REL_AMD64_TOKEN = 0x000D,
  IMAGE_REL_AMD64_SREL32 = 0x000E,
  IMAGE_REL_AMD64_PAIR = 0x000F,
  IMAGE_REL_AMD64_SSPAN32 = 0x0010,
  IMAGE_REL_AMD64_EHANDLER = 0x0011
};

enum RelocationTypesARM : unsigned {
  IMAGE_REL_ARM_ABSOLUTE = 0x0000,
  IMAGE_REL_ARM_ADDR32 = 0x0001,
  IMAGE_REL_ARM_ADDR32NB = 0x0002,
  IMAGE_REL_ARM_BRANCH24 = 0x0003,
  IMAGE_REL_ARM_BRANCH11 = 0x0004,
  IMAGE_REL_ARM_TOKEN = 0x0005,
  IMAGE_REL_ARM_BLX24 = 0x0008,
  IMAGE_REL_ARM_BLX11 = 0x0009,
  IMAGE_REL_ARM_REL32 = 0x000A,
  IMAGE_REL_ARM_SECTION = 0x000E,
  IMAGE_REL_ARM_SECREL = 0x000F,
  IMAGE_REL_ARM_MOV32A = 0x0010,
  IMAGE_REL_ARM_MOV32T = 0x0011,
  IMAGE_REL_ARM_BRANCH20T = 0x0012,
  IMAGE_REL_ARM_BRANCH24T = 0x0014,
  IMAGE_REL_ARM_BLX23T = 0x0015,
  IMAGE_REL_ARM_PAIR = 0x0016,
};

enum RelocationTypesARM64 : unsigned {
  IMAGE_REL_ARM64_ABSOLUTE = 0x0000,
  IMAGE_REL_ARM64_ADDR32 = 0x0001,
  IMAGE_REL_ARM64_ADDR32NB = 0x0002,
  IMAGE_REL_ARM64_BRANCH26 = 0x0003,
  IMAGE_REL_ARM64_PAGEBASE_REL21 = 0x0004,
  IMAGE_REL_ARM64_REL21 = 0x0005,
  IMAGE_REL_ARM64_PAGEOFFSET_12A = 0x0006,
  IMAGE_REL_ARM64_PAGEOFFSET_12L = 0x0007,
  IMAGE_REL_ARM64_SECREL = 0x0008,
  IMAGE_REL_ARM64_SECREL_LOW12A = 0x0009,
  IMAGE_REL_ARM64_SECREL_HIGH12A = 0x000A,
  IMAGE_REL_ARM64_SECREL_LOW12L = 0x000B,
  IMAGE_REL_ARM64_TOKEN = 0x000C,
  IMAGE_REL_ARM64_SECTION = 0x000D,
  IMAGE_REL_ARM64_ADDR64 = 0x000E,
  IMAGE_REL_ARM64_BRANCH19 = 0x000F,
  IMAGE_REL_ARM64_BRANCH14 = 0x0010,
  IMAGE_REL_ARM64_REL32 = 0x0011,
  IMAGE_REL_ARM64_PAIR = 0x0012,
};

enum RelocationTypesMips : unsigned {
  IMAGE_REL_MIPS_ABSOLUTE = 0x0000,
  IMAGE_REL_MIPS_REFHALF = 0x0001,
  IMAGE_REL_MIPS_REFWORD = 0x0002,
  IMAGE_REL_MIPS_JMPADDR = 0x0003,
  IMAGE_REL_MIPS_REFHI = 0x0004,
  IMAGE_REL_MIPS_REFLO = 0x0005,
  IMAGE_REL_MIPS_GPREL = 0x0006,
  IMAGE_REL_MIPS_LITERAL = 0x0007,
  IMAGE_REL_MIPS_SECTION = 0x000A,
  IMAGE_REL_MIPS_SECREL = 0x000B,
  IMAGE_REL_MIPS_SECRELLO = 0x000C,
  IMAGE_REL_MIPS_SECRELHI = 0x000D,
  IMAGE_REL_MIPS_JMPADDR16 = 0x0010,
  IMAGE_REL_MIPS_REFWORDNB = 0x0022,
  IMAGE_REL_MIPS_PAIR = 0x0025,
};

/// Relocation types of AMD64 and ARM64 objects that only LLD applies. A number
/// has one meaning on both machines, and a type is named on the machines that
/// have it.
enum RelocationTypesLLVM : unsigned {
#define COFF_LLVM_RELOC(Name, Value, Props, Use) IMAGE_REL_LLVM_##Name = Value,
#include "llvm/BinaryFormat/COFFRelocs/LLVM.def"
#undef COFF_LLVM_RELOC
  /// An AMD64 REL32 or REL32_1 to REL32_5 at the field of an instruction
  /// relative to rip that no other type states: bits 4 to 7 of the type are
  /// the bytes of the instruction before the field, and bits 0 to 3 those of
  /// its immediate after it.
  IMAGE_REL_LLVM_REL32_INSN = 0x8100,
};

/// Returns true for the types of IMAGE_REL_LLVM_REL32_INSN.
inline bool isRel32Insn(uint16_t Type) {
  return (Type & 0xff00) == IMAGE_REL_LLVM_REL32_INSN;
}

/// What COFFRelocs/LLVM.def says of a type of RelocationTypesLLVM.
namespace LLVMReloc {
enum : uint8_t {
  /// The machines that have the type.
  AMD64 = 1,
  ARM64 = 2,
  Both = AMD64 | ARM64,
  /// Its SymbolTableIndex names no symbol (relocationNamesNoSymbol).
  NoSymbol = 4,
  /// The linker may change the size of the code at it, on the machines that
  /// have it (relocationMayResizeCode).
  Resizes = 8,
  /// The linker moves the AMD64 code after it (relocationMovesCode).
  Moves = 16,
  /// It may take the address of the function it names
  /// (isGuardCFAddressTakingRelocation).
  Address = 32,
  /// It is a fused site or a fused read, which bind only where the assembler
  /// checked them.
  Fused = 64,
};
} // namespace LLVMReloc

/// The owned thread-local model's names: each one's symbol, and for an export
/// of the runtime, Id##Import, the import symbol an image reads it through.
namespace TP {
#define TP_EXPORT(Id, Name, Sealed)                                        \
  inline constexpr char Id[] = #Name, Id##Import[] = "__imp_" #Name Sealed;
#define TP_SYMBOL(Id, Name) inline constexpr char Id[] = #Name;
#include "llvm/BinaryFormat/ThreadPointer.inc"
/// The address space whose 32-bit loads the compiler makes fault probes.
inline constexpr unsigned FaultProbeAddrSpace = 280;
/// The fast-fail code of a thread pointer reloaded from its register's save
/// slot that names no block: FAST_FAIL_STACK_COOKIE_CHECK_FAILURE, a frame
/// overwritten.
inline constexpr unsigned ThreadPointerCheckFailure = 2;
} // namespace TP

/// Returns the LLVMReloc properties of \p Type, or zero for a type that is
/// none of RelocationTypesLLVM.
inline uint8_t getLLVMRelocProperties(uint16_t Type) {
  using namespace LLVMReloc;
  if (isRel32Insn(Type))
    return AMD64;
  switch (Type) {
#define COFF_LLVM_RELOC(Name, Value, Props, Use)                               \
  case Value:                                                                  \
    return Props;
#include "llvm/BinaryFormat/COFFRelocs/LLVM.def"
#undef COFF_LLVM_RELOC
  }
  return 0;
}

/// Returns the LLVMReloc machine of \p Machine, or zero for a machine that has
/// no LLVM relocation types.
inline uint8_t getLLVMRelocMachine(uint16_t Machine) {
  return Machine == IMAGE_FILE_MACHINE_AMD64 ? LLVMReloc::AMD64
         : isAnyArm64(Machine)               ? LLVMReloc::ARM64
                                             : 0;
}

/// Returns the name of the LLVM relocation type \p Type where \p Machine has
/// it, and an empty string otherwise.
LLVM_ABI StringRef getLLVMRelocName(uint16_t Machine, uint16_t Type);

/// Returns the LLVM relocation type of \p Machine that \p Name names, or
/// zero.
LLVM_ABI uint16_t getLLVMRelocType(uint16_t Machine, StringRef Name);

/// Returns true for a relocation type whose SymbolTableIndex names no symbol
/// and must not be resolved: ABSOLUTE, type 0 on every machine, ALIGN, whose
/// index is its parameters, TLS_BASE, and FUSED_ADD, whose index is the
/// length of its lea in bits 0 to 3 and above them what the lea adds, with
/// its sign in the lowest of those bits.
inline bool relocationNamesNoSymbol(uint16_t Type) {
  return Type == 0 || getLLVMRelocProperties(Type) & LLVMReloc::NoSymbol;
}

/// How an instruction reads the register that a load of a pointer wrote,
/// where an object that LLVM writes for Windows Itanium or NT-POSIX states it
/// as a fused site.
enum class FusedUse : uint8_t {
  None,
  /// A copy of the whole register into another.
  Copy,
  /// A call through the register.
  Call,
  /// A jump through the register.
  Jump,
  /// The add of a constant to the register.
  Add,
  /// An access to memory at the register plus a constant.
  Access,
  /// The use of a word of a thread-local variable's record, which a marked
  /// access loaded into the register: of the address of the TLS index as an
  /// access does, and of an offset as one of the two registers that an
  /// instruction adds, in a memory operand or to give a third.
  Index,
  /// A test of the register for zero that one conditional jump alone reads.
  Test,
};

/// Returns how the instruction that a relocation of \p Type marks on
/// \p Machine reads the register of its pointer, or None for a type that
/// marks no fused site.
inline FusedUse getFusedMarkerUse(uint16_t Machine, uint16_t Type) {
  // An AMD64 FUSED_TEST is the field of a test of the pointer in memory.
  if (!(getLLVMRelocProperties(Type) & getLLVMRelocMachine(Machine)) ||
      (Type == IMAGE_REL_LLVM_FUSED_TEST &&
       Machine == IMAGE_FILE_MACHINE_AMD64))
    return FusedUse::None;
  switch (Type) {
#define COFF_LLVM_RELOC(Name, Value, Props, Use)                               \
  case Value:                                                                  \
    return FusedUse::Use;
#include "llvm/BinaryFormat/COFFRelocs/LLVM.def"
#undef COFF_LLVM_RELOC
  }
  return FusedUse::None;
}

/// Returns the first relocation type of \p Machine that marks a fused site
/// whose instruction reads its register as \p Use, or zero.
LLVM_ABI uint16_t getFusedMarker(uint16_t Machine, FusedUse Use);

/// The field of an AMD64 MEM_LOAD, MEM_LOAD_REX2, FUSED_READ or
/// FUSED_READ_REX2 relocation holds no addend. This bit of it says that the
/// flags are dead at the load, so that the linker may write a zero in the
/// load's place with an instruction that changes them.
enum : uint32_t { LoadFlagsDead = 0x20 };

/// The field of an AMD64 MEM_JUMP_TAIL relocation holds no addend. This bit
/// of it says that a jcc with an 8-bit displacement directly before the jmp
/// jumps over it and that nothing else reaches the jmp, so that the linker
/// may write the two as one jcc, of the opposite condition, to the function
/// the pointer names.
enum : uint32_t { JumpOverJcc = 1 };

/// Returns true if \p Name is that of a pointer the linker may bind: the
/// import pointer of a symbol, or the cell a compiler keeps for one.
inline bool isBoundPointerName(StringRef Name) {
  return Name.starts_with("__imp_") || Name.starts_with(".refptr.");
}

/// Returns true if \p Name is that of the record of a thread-local variable,
/// through which code reaches a variable that may be another image's.
inline bool isTlsRecordName(StringRef Name) { return Name.ends_with("$tls"); }

/// Returns true if \p Name is that of the pointer to such a record.
inline bool isTlsRecordPointerName(StringRef Name) {
  return Name.starts_with("__imp_") && isTlsRecordName(Name);
}

/// A record holds the address of its image's TLS index, 64 bits at offset 0,
/// and the offsets of the variable and of its guard, 32 bits at 8 and at 12.
/// Returns the size of the word at \p Offset, or 0 where none begins there.
inline unsigned getTlsRecordWordSize(int64_t Offset) {
  return Offset == 0 ? 8 : Offset == 8 || Offset == 12 ? 4 : 0;
}

/// An instruction that adds two registers names them in two operands, and a
/// fused index reads one of them. Returns the second, where \p Op is the
/// first: the next operand of \p I that is a register.
template <class InstT>
unsigned getFusedIndexOperand(const InstT &I, unsigned Op) {
  do
    ++Op;
  while (!I.getOperand(Op).isReg());
  return Op;
}

/// What an object for Windows Itanium or NT-POSIX states of the site of one
/// relocation in the SiteFacts group of its link records, where the
/// assembler checked its fused relocation types.
enum SiteFact : uint64_t {
  /// At a load of the import pointer of a constant that every image defines
  /// alike, at the instruction that gives it the pointer's page and at the
  /// marker of its fused sites: the code behaves the same given the address
  /// of any definition, so the linker may bind these together to the
  /// image's own.
  SiteFactValueOnly = 7,
  /// At a FUSED_INDEX: the word of the record that the register holds, its
  /// offset in the record, plus SiteFactViaFirst where the register is the
  /// first of the two that the instruction adds.
  SiteFactIndex = 8,
  /// At the marker of a fused access that loads one word of a record: only
  /// fused indexes of the function's code read the register it writes.
  SiteFactIndexedRead = 9,
  /// At a FUSED_INDEX of a record's word that the instruction adds to the
  /// base an owned-model record chain selected: the register, by its
  /// encoding, that the chain's thread pointer plus the place is formed from,
  /// which holds the thread pointer there but for the chain's select writing
  /// it, so the linker may add the word to that.
  SiteFactThreadPointer = 14,
  /// At the marker of a fused access that only reads such a constant through
  /// a load that stays: the linker may bind it alone to the image's own
  /// definition, which it then reads with no register.
  SiteFactValueOnlyAccess = 12,
  /// At the add of either part of a thread-local's offset: the instruction
  /// after it reads the register the add writes, and none after that reads
  /// what the add wrote, so the linker may delete the add of a part that is
  /// zero and give its base to that instruction.
  SiteFactOnlyNextReads = 15,
};
enum : uint64_t { SiteFactViaFirst = 16 };
/// At a FUSED_INDEX of an owned-model record chain, SiteFactIndex's word may
/// instead state what the chain derived into the register: the place, the
/// thread pointer plus it, the wide word or a step decoding it, the key, and
/// on ARM64 the form word in a register of its own.
enum : uint64_t {
  SiteFactIndexPlace = 1,
  SiteFactIndexBase = 2,
  SiteFactIndexWide = 3,
  SiteFactIndexKey = 4,
  SiteFactIndexForm = 5,
};

/// Returns true if the linker may change the size of the code at a relocation
/// of \p Type against \p Symbol in an object that LLVM writes for Windows
/// Itanium or NT-POSIX on \p Machine, AMD64 or ARM64: a jump whose width it
/// chooses or which it deletes, a call or jump through a pointer that it
/// binds, a 64-bit load of such a pointer, which it may write as a shorter
/// zero or constant, such a load with the call through its register, a fused
/// site, a test whose outcome it knows, a thread-local base, alignment
/// padding, an ARM64 adrp and the instruction after it where it may fold the
/// two into one, an ARM64 load of a word of a thread-local variable's record
/// or of the key through its import pointer, and a fused index. \p Pair holds
/// the adrp's word in its low half and the other's in its high half, which
/// folds where it is an add, at PAGEOFFSET_12A, or a load of a word, signed
/// word or doubleword into an integer register, at PAGEOFFSET_12L, of the
/// address into the adrp's register, its field the low 12 bits of the adrp's
/// addend, so that the two compute the symbol plus that addend. It is also
/// the add, with LSL #12, of a thread-local's high part and the instruction
/// after it, the add deleted where the part is zero: an add or an
/// unsigned-offset load or store whose base is the add's register and whose
/// field, scaled, holds the add's, the low 12 bits of the addend; and the
/// add of a checked low part, which has no high part, and an add or a load
/// or store with that base. A load of
/// any other symbol keeps its size, and the linker resizes
/// code at no other relocation. The assembler resolves no distance across such
/// code where the linker moves it; in an AMD64 section that it only compacts,
/// it states the values that depend on the layout as spans.
inline bool relocationMayResizeCode(uint16_t Machine, uint16_t Type,
                                    StringRef Symbol = {}, uint64_t Pair = 0) {
  bool AMD64 = Machine == IMAGE_FILE_MACHINE_AMD64;
  if (AMD64 &&
      (Type == IMAGE_REL_LLVM_MEM_LOAD || Type == IMAGE_REL_LLVM_MEM_LOAD_REX2))
    return isBoundPointerName(Symbol);
  if (Type == IMAGE_REL_LLVM_FUSED_INDEX)
    return true;
  if (!AMD64 && (Type == IMAGE_REL_ARM64_PAGEBASE_REL21 ||
                 Type == IMAGE_REL_ARM64_PAGEOFFSET_12A ||
                 Type == IMAGE_REL_ARM64_PAGEOFFSET_12L)) {
    // add Xd, Xd, #lo12; ldr Wd, ldrsw Xd or ldr Xd, [Xd, #lo12].
    uint32_t Adrp = Pair, Low = Pair >> 32, Reg = Adrp & 31;
    uint32_t Op = (Low & ~0x3ffc00u) ^ (Reg << 5 | Reg);
    bool Load = Op == 0xb9400000 || Op == 0xb9800000 || Op == 0xf9400000;
    uint32_t Lo12 = (Low >> 10 & 0xfff) << (Load ? Low >> 30 : 0);
    return (Adrp & 0x9f000000) == 0x90000000 && Reg != 31 &&
           Lo12 == ((Adrp >> 29 & 3) | (Adrp >> 3 & 0xffc)) &&
           ((Type != IMAGE_REL_ARM64_PAGEOFFSET_12L && Op == 0x91000000) ||
            (Type != IMAGE_REL_ARM64_PAGEOFFSET_12A && Load));
  }
  bool Lone = Type == IMAGE_REL_LLVM_DTPOFF_LOW12A ||
              Type == IMAGE_REL_LLVM_TPOFF_LOW12A;
  if (!AMD64 && (Lone || Type == IMAGE_REL_ARM64_SECREL_HIGH12A ||
                 Type == IMAGE_REL_LLVM_TPOFF_HIGH12A)) {
    // add Xd, Xn, #a, lsl #12; add Xm, Xd, #a or ld/st [Xd, #a]. A lone
    // low part: add Xd, Xn, #a; add Xm, Xd, #b, ld/st [Xd, #b] or [Xd, Rm].
    uint32_t Hi = Pair, Low = Pair >> 32, Reg = Hi & 31;
    bool Add = (Low & 0xffc003e0) == (0x91000000 | Reg << 5);
    bool Access = (Low & 0x3b0003e0) == (0x39000000 | Reg << 5);
    bool Index = (Low & 0x3b200fe0) == (0x38200800 | Reg << 5);
    uint32_t Scale =
        Access ? Low >> 30 | ((Low & 0x4800000) == 0x4800000) << 2 : 0;
    return (Hi & 0xffc00000) == (Lone ? 0x91000000 : 0x91400000) &&
           Reg != 31 &&
           (Lone ? Add || Access || Index
                 : (Add || Access) &&
                       (Low >> 10 & 0xfff) << Scale == (Hi >> 10 & 0xfff));
  }
  if (!AMD64 && Type == IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL)
    return isTlsRecordPointerName(Symbol) ||
           (Symbol.consume_front("__imp_") && Symbol == TP::Key);
  uint8_t Props = getLLVMRelocProperties(Type);
  return Props & LLVMReloc::Resizes &&
         Props & (AMD64 ? LLVMReloc::AMD64 : LLVMReloc::ARM64);
}

/// Returns true if the linker moves the code after a relocation of the AMD64
/// \p Type: its rewrite removes an instruction or changes the length of one
/// that no other form replaces in place, which a fused site and a
/// thread-local base do. The linker moves the code of an AMD64 section freely
/// only where the section has such a relocation. Any other it compacts by the
/// spans it states: it deletes the bytes that a rewrite frees, such as those
/// of a load whose call through its register becomes a direct call, narrows
/// the jumps that the spans name and shortens the jump that ends it.
inline bool relocationMovesCode(uint16_t Type) {
  return getLLVMRelocProperties(Type) & LLVMReloc::Moves;
}

/// Returns true if the relocations in section \p SectionName may take the
/// address of the functions they name, which Control Flow Guard then lists
/// as indirect call targets. Unwind data and debug info only describe the
/// functions they name, and the candidates of a function override are
/// reached only by the branches Windows rewrites.
LLVM_ABI bool isGuardCFAddressTakingSection(StringRef SectionName);

/// Returns true if a relocation of \p Type in an object for \p Machine may
/// take the address of the function it names, as
/// isGuardCFAddressTakingSection counts it. i386's REL32 and ARM64's branch
/// types only branch; x86-64's REL32 also computes addresses. An LLVM type
/// states whether it does.
LLVM_ABI bool isGuardCFAddressTakingRelocation(uint16_t Machine, uint16_t Type);

enum DynamicRelocationType : unsigned {
  IMAGE_DYNAMIC_RELOCATION_GUARD_RF_PROLOGUE = 1,
  IMAGE_DYNAMIC_RELOCATION_GUARD_RF_EPILOGUE = 2,
  IMAGE_DYNAMIC_RELOCATION_GUARD_IMPORT_CONTROL_TRANSFER = 3,
  IMAGE_DYNAMIC_RELOCATION_GUARD_INDIR_CONTROL_TRANSFER = 4,
  IMAGE_DYNAMIC_RELOCATION_GUARD_SWITCHTABLE_BRANCH = 5,
  IMAGE_DYNAMIC_RELOCATION_ARM64X = 6,
  IMAGE_DYNAMIC_RELOCATION_FUNCTION_OVERRIDE = 7,
};

enum FunctionOverrideFixupType : uint8_t {
  IMAGE_FUNCTION_OVERRIDE_INVALID = 0,
  IMAGE_FUNCTION_OVERRIDE_X64_REL32 = 1,
  IMAGE_FUNCTION_OVERRIDE_ARM64_BRANCH26 = 2,
  IMAGE_FUNCTION_OVERRIDE_ARM64_THUNK = 3,
};

enum Arm64XFixupType : uint8_t {
  IMAGE_DVRT_ARM64X_FIXUP_TYPE_ZEROFILL = 0,
  IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE = 1,
  IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA = 2,
};

/// The KCFI type reserved for a function that no pointer can reach, under
/// function type prefixes. No function or call has it: a type that hashes to
/// it is one more.
enum : uint32_t { SealedTypeId = 0 };

/// Under function type prefixes a type is 31 bits, and a prefix whose function
/// has an entry that is passed the thread pointer stores its type with this bit
/// set, and the entry's offset from the function in the word before its
/// marker.
enum : uint32_t { ThreadPointerTypeTag = 0x80000000 };

enum COMDATType : uint8_t {
  IMAGE_COMDAT_SELECT_NODUPLICATES = 1,
  IMAGE_COMDAT_SELECT_ANY,
  IMAGE_COMDAT_SELECT_SAME_SIZE,
  IMAGE_COMDAT_SELECT_EXACT_MATCH,
  IMAGE_COMDAT_SELECT_ASSOCIATIVE,
  IMAGE_COMDAT_SELECT_LARGEST,
  IMAGE_COMDAT_SELECT_NEWEST
};

// Auxiliary Symbol Formats
struct AuxiliaryFunctionDefinition {
  uint32_t TagIndex;
  uint32_t TotalSize;
  uint32_t PointerToLinenumber;
  uint32_t PointerToNextFunction;
  char unused[2];
};

struct AuxiliarybfAndefSymbol {
  uint8_t unused1[4];
  uint16_t Linenumber;
  uint8_t unused2[6];
  uint32_t PointerToNextFunction;
  uint8_t unused3[2];
};

struct AuxiliaryWeakExternal {
  uint32_t TagIndex;
  uint32_t Characteristics;
  uint8_t unused[10];
};

enum WeakExternalCharacteristics : unsigned {
  IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY = 1,
  IMAGE_WEAK_EXTERN_SEARCH_LIBRARY = 2,
  IMAGE_WEAK_EXTERN_SEARCH_ALIAS = 3,
  IMAGE_WEAK_EXTERN_ANTI_DEPENDENCY = 4
};

struct AuxiliarySectionDefinition {
  uint32_t Length;
  uint16_t NumberOfRelocations;
  uint16_t NumberOfLinenumbers;
  uint32_t CheckSum;
  uint32_t Number;
  uint8_t Selection;
  char unused;
};

struct AuxiliaryCLRToken {
  uint8_t AuxType;
  uint8_t unused1;
  uint32_t SymbolTableIndex;
  char unused2[12];
};

union Auxiliary {
  AuxiliaryFunctionDefinition FunctionDefinition;
  AuxiliarybfAndefSymbol bfAndefSymbol;
  AuxiliaryWeakExternal WeakExternal;
  AuxiliarySectionDefinition SectionDefinition;
};

/// The Import Directory Table.
///
/// There is a single array of these and one entry per imported DLL.
struct ImportDirectoryTableEntry {
  uint32_t ImportLookupTableRVA;
  uint32_t TimeDateStamp;
  uint32_t ForwarderChain;
  uint32_t NameRVA;
  uint32_t ImportAddressTableRVA;
};

/// The PE32 Import Lookup Table.
///
/// There is an array of these for each imported DLL. It represents either
/// the ordinal to import from the target DLL, or a name to lookup and import
/// from the target DLL.
///
/// This also happens to be the same format used by the Import Address Table
/// when it is initially written out to the image.
struct ImportLookupTableEntry32 {
  uint32_t data;

  /// Is this entry specified by ordinal, or name?
  bool isOrdinal() const { return data & 0x80000000; }

  /// Get the ordinal value of this entry. isOrdinal must be true.
  uint16_t getOrdinal() const {
    assert(isOrdinal() && "ILT entry is not an ordinal!");
    return data & 0xFFFF;
  }

  /// Set the ordinal value and set isOrdinal to true.
  void setOrdinal(uint16_t o) {
    data = o;
    data |= 0x80000000;
  }

  /// Get the Hint/Name entry RVA. isOrdinal must be false.
  uint32_t getHintNameRVA() const {
    assert(!isOrdinal() && "ILT entry is not a Hint/Name RVA!");
    return data;
  }

  /// Set the Hint/Name entry RVA and set isOrdinal to false.
  void setHintNameRVA(uint32_t rva) { data = rva; }
};

/// The DOS compatible header at the front of all PEs.
struct DOSHeader {
  uint16_t Magic;
  uint16_t UsedBytesInTheLastPage;
  uint16_t FileSizeInPages;
  uint16_t NumberOfRelocationItems;
  uint16_t HeaderSizeInParagraphs;
  uint16_t MinimumExtraParagraphs;
  uint16_t MaximumExtraParagraphs;
  uint16_t InitialRelativeSS;
  uint16_t InitialSP;
  uint16_t Checksum;
  uint16_t InitialIP;
  uint16_t InitialRelativeCS;
  uint16_t AddressOfRelocationTable;
  uint16_t OverlayNumber;
  uint16_t Reserved[4];
  uint16_t OEMid;
  uint16_t OEMinfo;
  uint16_t Reserved2[10];
  uint32_t AddressOfNewExeHeader;
};

struct PE32Header {
  enum { PE32 = 0x10b, PE32_PLUS = 0x20b };

  uint16_t Magic;
  uint8_t MajorLinkerVersion;
  uint8_t MinorLinkerVersion;
  uint32_t SizeOfCode;
  uint32_t SizeOfInitializedData;
  uint32_t SizeOfUninitializedData;
  uint32_t AddressOfEntryPoint; // RVA
  uint32_t BaseOfCode;          // RVA
  uint32_t BaseOfData;          // RVA
  uint64_t ImageBase;
  uint32_t SectionAlignment;
  uint32_t FileAlignment;
  uint16_t MajorOperatingSystemVersion;
  uint16_t MinorOperatingSystemVersion;
  uint16_t MajorImageVersion;
  uint16_t MinorImageVersion;
  uint16_t MajorSubsystemVersion;
  uint16_t MinorSubsystemVersion;
  uint32_t Win32VersionValue;
  uint32_t SizeOfImage;
  uint32_t SizeOfHeaders;
  uint32_t CheckSum;
  uint16_t Subsystem;
  // FIXME: This should be DllCharacteristics to match the COFF spec.
  uint16_t DLLCharacteristics;
  uint64_t SizeOfStackReserve;
  uint64_t SizeOfStackCommit;
  uint64_t SizeOfHeapReserve;
  uint64_t SizeOfHeapCommit;
  uint32_t LoaderFlags;
  // FIXME: This should be NumberOfRvaAndSizes to match the COFF spec.
  uint32_t NumberOfRvaAndSize;
};

struct DataDirectory {
  uint32_t RelativeVirtualAddress;
  uint32_t Size;
};

enum DataDirectoryIndex : unsigned {
  EXPORT_TABLE = 0,
  IMPORT_TABLE,
  RESOURCE_TABLE,
  EXCEPTION_TABLE,
  CERTIFICATE_TABLE,
  BASE_RELOCATION_TABLE,
  DEBUG_DIRECTORY,
  ARCHITECTURE,
  GLOBAL_PTR,
  TLS_TABLE,
  LOAD_CONFIG_TABLE,
  BOUND_IMPORT,
  IAT,
  DELAY_IMPORT_DESCRIPTOR,
  CLR_RUNTIME_HEADER,

  NUM_DATA_DIRECTORIES
};

enum WindowsSubsystem : unsigned {
  IMAGE_SUBSYSTEM_UNKNOWN = 0, ///< An unknown subsystem.
  IMAGE_SUBSYSTEM_NATIVE = 1,  ///< Device drivers and native Windows processes
  IMAGE_SUBSYSTEM_WINDOWS_GUI = 2,      ///< The Windows GUI subsystem.
  IMAGE_SUBSYSTEM_WINDOWS_CUI = 3,      ///< The Windows character subsystem.
  IMAGE_SUBSYSTEM_OS2_CUI = 5,          ///< The OS/2 character subsystem.
  IMAGE_SUBSYSTEM_POSIX_CUI = 7,        ///< The POSIX character subsystem.
  IMAGE_SUBSYSTEM_NATIVE_WINDOWS = 8,   ///< Native Windows 9x driver.
  IMAGE_SUBSYSTEM_WINDOWS_CE_GUI = 9,   ///< Windows CE.
  IMAGE_SUBSYSTEM_EFI_APPLICATION = 10, ///< An EFI application.
  IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER = 11, ///< An EFI driver with boot
                                                ///  services.
  IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER = 12,      ///< An EFI driver with run-time
                                                ///  services.
  IMAGE_SUBSYSTEM_EFI_ROM = 13,                 ///< An EFI ROM image.
  IMAGE_SUBSYSTEM_XBOX = 14,                    ///< XBOX.
  IMAGE_SUBSYSTEM_WINDOWS_BOOT_APPLICATION = 16 ///< A BCD application.
};

enum DLLCharacteristics : unsigned {
  /// ASLR with 64 bit address space.
  IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA = 0x0020,
  /// DLL can be relocated at load time.
  IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE = 0x0040,
  /// Code integrity checks are enforced.
  IMAGE_DLL_CHARACTERISTICS_FORCE_INTEGRITY = 0x0080,
  ///< Image is NX compatible.
  IMAGE_DLL_CHARACTERISTICS_NX_COMPAT = 0x0100,
  /// Isolation aware, but do not isolate the image.
  IMAGE_DLL_CHARACTERISTICS_NO_ISOLATION = 0x0200,
  /// Does not use structured exception handling (SEH). No SEH handler may be
  /// called in this image.
  IMAGE_DLL_CHARACTERISTICS_NO_SEH = 0x0400,
  /// Do not bind the image.
  IMAGE_DLL_CHARACTERISTICS_NO_BIND = 0x0800,
  ///< Image should execute in an AppContainer.
  IMAGE_DLL_CHARACTERISTICS_APPCONTAINER = 0x1000,
  ///< A WDM driver.
  IMAGE_DLL_CHARACTERISTICS_WDM_DRIVER = 0x2000,
  ///< Image supports Control Flow Guard.
  IMAGE_DLL_CHARACTERISTICS_GUARD_CF = 0x4000,
  /// Terminal Server aware.
  IMAGE_DLL_CHARACTERISTICS_TERMINAL_SERVER_AWARE = 0x8000
};

enum ExtendedDLLCharacteristics : unsigned {
  /// Image is CET compatible
  IMAGE_DLL_CHARACTERISTICS_EX_CET_COMPAT = 0x0001,
  /// Image is CET compatible in strict mode
  IMAGE_DLL_CHARACTERISTICS_EX_CET_COMPAT_STRICT_MODE = 0x0002,
  /// Image is CET compatible in such a way that context IP validation is
  /// relaxed
  IMAGE_DLL_CHARACTERISTICS_EX_CET_SET_CONTEXT_IP_VALIDATION_RELAXED_MODE =
      0x0004,
  /// Image is CET compatible in such a way that the use of
  /// dynamic APIs is restricted to processes only
  IMAGE_DLL_CHARACTERISTICS_EX_CET_DYNAMIC_APIS_ALLOW_IN_PROC_ONLY = 0x0008,
  /// Reserved for future use. Not used by MSVC link.exe
  IMAGE_DLL_CHARACTERISTICS_EX_CET_RESERVED_1 = 0x0010,
  /// Reserved for future use. Not used by MSVC link.exe
  IMAGE_DLL_CHARACTERISTICS_EX_CET_RESERVED_2 = 0x0020,
  /// Image is CFI compatible.
  IMAGE_DLL_CHARACTERISTICS_EX_FORWARD_CFI_COMPAT = 0x0040,
  /// Image is hotpatch compatible.
  IMAGE_DLL_CHARACTERISTICS_EX_HOTPATCH_COMPATIBLE = 0x0080,
};

enum DebugType : unsigned {
  IMAGE_DEBUG_TYPE_UNKNOWN = 0,
  IMAGE_DEBUG_TYPE_COFF = 1,
  IMAGE_DEBUG_TYPE_CODEVIEW = 2,
  IMAGE_DEBUG_TYPE_FPO = 3,
  IMAGE_DEBUG_TYPE_MISC = 4,
  IMAGE_DEBUG_TYPE_EXCEPTION = 5,
  IMAGE_DEBUG_TYPE_FIXUP = 6,
  IMAGE_DEBUG_TYPE_OMAP_TO_SRC = 7,
  IMAGE_DEBUG_TYPE_OMAP_FROM_SRC = 8,
  IMAGE_DEBUG_TYPE_BORLAND = 9,
  IMAGE_DEBUG_TYPE_RESERVED10 = 10,
  IMAGE_DEBUG_TYPE_CLSID = 11,
  IMAGE_DEBUG_TYPE_VC_FEATURE = 12,
  IMAGE_DEBUG_TYPE_POGO = 13,
  IMAGE_DEBUG_TYPE_ILTCG = 14,
  IMAGE_DEBUG_TYPE_MPX = 15,
  IMAGE_DEBUG_TYPE_REPRO = 16,
  IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS = 20,
};

enum BaseRelocationType : unsigned {
  IMAGE_REL_BASED_ABSOLUTE = 0,
  IMAGE_REL_BASED_HIGH = 1,
  IMAGE_REL_BASED_LOW = 2,
  IMAGE_REL_BASED_HIGHLOW = 3,
  IMAGE_REL_BASED_HIGHADJ = 4,
  IMAGE_REL_BASED_MIPS_JMPADDR = 5,
  IMAGE_REL_BASED_ARM_MOV32A = 5,
  IMAGE_REL_BASED_ARM_MOV32T = 7,
  IMAGE_REL_BASED_MIPS_JMPADDR16 = 9,
  IMAGE_REL_BASED_DIR64 = 10
};

enum ImportType : unsigned {
  IMPORT_CODE = 0,
  IMPORT_DATA = 1,
  IMPORT_CONST = 2
};

enum ImportNameType : unsigned {
  /// Import is by ordinal. This indicates that the value in the Ordinal/Hint
  /// field of the import header is the import's ordinal. If this constant is
  /// not specified, then the Ordinal/Hint field should always be interpreted
  /// as the import's hint.
  IMPORT_ORDINAL = 0,
  /// The import name is identical to the public symbol name
  IMPORT_NAME = 1,
  /// The import name is the public symbol name, but skipping the leading ?,
  /// @, or optionally _.
  IMPORT_NAME_NOPREFIX = 2,
  /// The import name is the public symbol name, but skipping the leading ?,
  /// @, or optionally _, and truncating at the first @.
  IMPORT_NAME_UNDECORATE = 3,
  /// The import name is specified as a separate string in the import library
  /// object file.
  IMPORT_NAME_EXPORTAS = 4
};

enum class GuardFlags : uint32_t {
  /// Module performs control flow integrity checks using system-supplied
  /// support.
  CF_INSTRUMENTED = 0x100,
  /// Module performs control flow and write integrity checks.
  CFW_INSTRUMENTED = 0x200,
  /// Module contains valid control flow target metadata.
  CF_FUNCTION_TABLE_PRESENT = 0x400,
  /// Module does not make use of the /GS security cookie.
  SECURITY_COOKIE_UNUSED = 0x800,
  /// Module supports read only delay load IAT.
  PROTECT_DELAYLOAD_IAT = 0x1000,
  /// Delayload import table in its own .didat section (with nothing else in it)
  /// that can be freely reprotected.
  DELAYLOAD_IAT_IN_ITS_OWN_SECTION = 0x2000,
  /// Module contains suppressed export information. This also infers that the
  /// address taken IAT table is also present in the load config.
  CF_EXPORT_SUPPRESSION_INFO_PRESENT = 0x4000,
  /// Module enables suppression of exports.
  CF_ENABLE_EXPORT_SUPPRESSION = 0x8000,
  /// Module contains longjmp target information.
  CF_LONGJUMP_TABLE_PRESENT = 0x10000,
  /// Module contains EH continuation target information.
  EH_CONTINUATION_TABLE_PRESENT = 0x400000,
  /// Mask for the subfield that contains the stride of Control Flow Guard
  /// function table entries (that is, the additional count of bytes per table
  /// entry).
  CF_FUNCTION_TABLE_SIZE_MASK = 0xF0000000,
  CF_FUNCTION_TABLE_SIZE_5BYTES = 0x10000000,
  CF_FUNCTION_TABLE_SIZE_6BYTES = 0x20000000,
  CF_FUNCTION_TABLE_SIZE_7BYTES = 0x30000000,
  CF_FUNCTION_TABLE_SIZE_8BYTES = 0x40000000,
  CF_FUNCTION_TABLE_SIZE_9BYTES = 0x50000000,
  CF_FUNCTION_TABLE_SIZE_10BYTES = 0x60000000,
  CF_FUNCTION_TABLE_SIZE_11BYTES = 0x70000000,
  CF_FUNCTION_TABLE_SIZE_12BYTES = 0x80000000,
  CF_FUNCTION_TABLE_SIZE_13BYTES = 0x90000000,
  CF_FUNCTION_TABLE_SIZE_14BYTES = 0xA0000000,
  CF_FUNCTION_TABLE_SIZE_15BYTES = 0xB0000000,
  CF_FUNCTION_TABLE_SIZE_16BYTES = 0xC0000000,
  CF_FUNCTION_TABLE_SIZE_17BYTES = 0xD0000000,
  CF_FUNCTION_TABLE_SIZE_18BYTES = 0xE0000000,
  CF_FUNCTION_TABLE_SIZE_19BYTES = 0xF0000000,
};

/// The flags byte of a Control Flow Guard function table entry, present when
/// GuardFlags gives the table a stride.
enum class GuardFidFlags : uint8_t {
  /// The function's address is taken, but it is not a valid call target.
  FID_SUPPRESSED = 0x1,
  /// The function is an export that becomes a valid call target only when the
  /// loader resolves it dynamically, under export suppression.
  EXPORT_SUPPRESSED = 0x2,
};

struct ImportHeader {
  uint16_t Sig1; ///< Must be IMAGE_FILE_MACHINE_UNKNOWN (0).
  uint16_t Sig2; ///< Must be 0xFFFF.
  uint16_t Version;
  uint16_t Machine;
  uint32_t TimeDateStamp;
  uint32_t SizeOfData;
  uint16_t OrdinalHint;
  uint16_t TypeInfo;

  ImportType getType() const { return static_cast<ImportType>(TypeInfo & 0x3); }

  ImportNameType getNameType() const {
    return static_cast<ImportNameType>((TypeInfo & 0x1C) >> 2);
  }
};

enum CodeViewIdentifiers {
  DEBUG_SECTION_MAGIC = 0x4,
  DEBUG_HASHES_SECTION_MAGIC = 0x133C9C5
};

// These flags show up in the @feat.00 symbol. They appear to be some kind of
// compiler features bitfield read by link.exe.
enum Feat00Flags : uint32_t {
  // Object is compatible with /safeseh.
  SafeSEH = 0x1,
  // Object was compiled with /GS.
  GuardStack = 0x100,
  // Object was compiled with /sdl.
  SDL = 0x200,
  // Object was compiled with /guard:cf.
  GuardCF = 0x800,
  // Object was compiled with /guard:ehcont.
  GuardEHCont = 0x4000,
  // Object was compiled with /kernel.
  Kernel = 0x40000000,
};

/// The value of the @comp.id symbol in an object that LLVM writes for Windows
/// Itanium or NT-POSIX: the product in the high half, no build number.
enum : uint32_t { LLVMCompId = 0x4C4C0000 };

/// A .llvm_link_records section holds LinkRecordsMagic, a version and flags,
/// then groups, each a kind, the size of its payload and the payload. Every
/// number is a ULEB128. A reader skips a group whose kind it does not know,
/// unless the kind is critical.
static const char LinkRecordsMagic[] = {'L', 'L', 'R', 'C'};
enum : uint64_t { LinkRecordsVersion = 1, LinkRecordCritical = 1 };

/// A flag of a .llvm_link_records section: the assembler checked the object's
/// fused relocation types against its instructions.
enum : uint64_t { LinkRecordsFusedChecked = 8 };

enum LinkRecordKind : uint64_t {
  // In an object.
  LinkRecordSiteFacts = 0,
  LinkRecordSymbols = 1,
  LinkRecordSections = 2,
  LinkRecordContracts = 3,
  LinkRecordSpans = 5,
  // In a member of an import library.
  LinkRecordImages = 4,
  LinkRecordOverrides = 6,
  LinkRecordImageCode = 8,
  LinkRecordScan = 10,
};

/// The payload of a Symbols group is link facts, each the index of a symbol,
/// a tag, the size of its operands and the operands. A reader skips a fact
/// whose tag it does not know, unless the tag is critical.
enum LinkFactTag : uint64_t {
  /// The function has a type prefix. The operand is the words the prefix
  /// holds before its marker, a set of LinkFactPrefixWord.
  LinkFactTypePrefix = 0,
  /// The visibility of the symbol, a LinkFactVisibilityState. Critical.
  /// Every name of a vague entity states it: of a definition that the images
  /// of a process share, each image binding the one that an image it imports
  /// from keeps, of an alias of one and of what shares its COMDAT. So does a
  /// weak reference whose visibility is not default, which no other image's
  /// definition satisfies, and each name of a function that states
  /// LinkFactOverridable.
  LinkFactVisibility = 1,
  /// The symbol is a variable that is a name of a vague entity. The operand
  /// is how it is initialized, a LinkFactInitKind.
  LinkFactInit = 2,
  /// The variable is a type_info object. Critical.
  LinkFactDescriptor = 3,
  /// The symbol is a function that is a name of a vague entity, whose
  /// address is significant and which this unit takes other than as a
  /// call's callee, or which is weak_odr and not exported, whose address a
  /// unit that has no copy may take. A unit states no other function.
  /// Critical.
  LinkFactAddressSignificant = 5,
  /// The symbol is a thread-local variable that another image may reach,
  /// which it does through a record of the variable that the linker writes.
  /// The operand is the symbol of the thread-local byte that is not zero once
  /// the variable is initialized in a thread. Critical.
  LinkFactTlsRecord = 7,
  /// The function is an overridable default, which a program replaces for
  /// every image, or a replacement of one. A default states its slot in the
  /// executable's override table, then, where it is specified as calling
  /// another default, that one's slot, then where that one takes its
  /// arguments without the second, 1 where the second is the last and 2
  /// where others follow it. A replacement states no operand. Critical.
  LinkFactOverridable = 9,
  /// The function is a weak definition with no slot, which a definition
  /// elsewhere in the program replaces by its name. Critical.
  LinkFactOverridableByName = 11,
};

/// The highest slot of the override table.
enum : uint64_t { LinkFactMaxOverrideSlot = 31 };

/// The states are in the order of constraint: the copies of one name have
/// the greatest state that one of them states.
enum LinkFactVisibilityState : uint64_t {
  /// Default visibility, and one instance in the process even where the
  /// image binds its own references to its definitions: GCC's unique
  /// binding, which a variable alone has.
  LinkFactUnique = 0,
  LinkFactDefault = 1,
  LinkFactProtected = 2,
  LinkFactHidden = 3,
};

/// From the least that a reader may assume of the variable to the most.
enum LinkFactInitKind : uint64_t {
  /// A constant with a constant initializer that no definition replaces.
  LinkFactConstantImmutable = 0,
  LinkFactConstantMutable = 1,
  /// Initialized under its guard variable when control first passes its
  /// declaration.
  LinkFactDynamicGuarded = 2,
  /// Initialized under its guard variable with its image.
  LinkFactDynamicUnordered = 3,
  /// A thread-local variable that each thread initializes when it first
  /// uses the variable.
  LinkFactTLSDynamic = 4,
};

/// The words before a type prefix's marker, in this order, each 4 bytes.
enum LinkFactPrefixWord : uint64_t {
  /// The type of the function as the target of a call through a pointer to
  /// a virtual member function.
  LinkFactVfnType = 1,
  /// The offset from the function of its entry that is passed the thread
  /// pointer.
  LinkFactThreadPointerEntry = 2,
};

/// In a Sections group, after the resizable sections, each dispatch through a
/// jump table is the symbols of its branch, its table and the label that the
/// entries are relative to, the number of entries, their
/// codeview::JumpTableEntrySize, the symbol of the instruction that loads
/// the entry and these flags.
enum SwitchTableFlags : uint64_t {
  /// The index is below the number of entries wherever the branch executes,
  /// by a check that the compiler emitted.
  SwitchTableRangeChecked = 1,
};

/// The payload of a Spans group is lists, each the symbol of the section that
/// holds its fields, the symbol of the code section whose layout they depend
/// on, a count and that many spans in the order of their fields. A span is
/// SpanKind plus sixteen times the distance of its field from the field of
/// the span before it, or from the section's start, and then the operand its
/// kind names. The field holds A - B, both points of the code section, which
/// the linker moves only by deleting bytes at relocations, by redoing padding
/// at ALIGN relocations and by narrowing SpanJump32 jumps; the linker reads A
/// from the field and writes the distance between where the two points come to
/// lie. A code section that a list of its own fields names, which may be empty,
/// states every such value in a list or a relocation.
enum SpanKind : unsigned {
  /// The 8-bit field of an instruction that ends with it, B.
  SpanRel8 = 0,
  /// The 32-bit field of an instruction that ends at B, with it or, in the
  /// kinds after this one but SpanJump32, 1, 2 or 4 bytes after it.
  SpanRel32 = 1,
  /// The 32-bit field of a jmp or jcc that ends with it, B, which the
  /// assembler made long and which reaches A with 8 bits in some link: the
  /// linker chooses its width.
  SpanJump32 = 4,
  /// A field of the relocation type SpanTypes gives for the kinds from this
  /// one on. The operand is B's offset.
  SpanData = 6,
  /// A 32-bit field in another section whose relocation names the code
  /// section's symbol: the field holds A's offset plus the operand, an
  /// SLEB128, and the linker adds A's movement to it.
  SpanAddend32 = 15,
};
constexpr uint16_t SpanTypes[] = {
    IMAGE_REL_LLVM_SSPAN8,        IMAGE_REL_LLVM_SSPAN16,
    IMAGE_REL_AMD64_SSPAN32,      IMAGE_REL_LLVM_SSPAN64,
    IMAGE_REL_LLVM_SSPAN_ULEB,    IMAGE_REL_LLVM_SSPAN_SLEB,
    IMAGE_REL_LLVM_USPAN8,        IMAGE_REL_LLVM_USPAN16,
    IMAGE_REL_LLVM_SSPAN_EPILOGV2};

/// The name of the one symbol of the member of a DLL's import library that
/// holds the DLL's link records. Nothing refers to it, so no link loads the
/// member: a linker that knows the records reads them where they are.
static const char LinkRecordsMemberSymbol[] = "__llvm_link_records$lib";

/// The payload of an Images group is a count and that many layout shapes,
/// each a name, which the type_info objects of its images name by index, then
/// a count and that many images: a DLL and then every image that is loaded
/// with it. Each is its name, a count and
/// that many imports, and a count and that many fields. A name is its size
/// and its bytes. An import is twice the index of an image of the group that
/// the image imports from, plus one where it loads that image late. A field
/// is a tag, the size of its payload and the payload, and a reader skips one
/// whose tag it does not know, unless the tag is critical.
enum LinkImageField : uint64_t {
  /// The vague entities that the image keeps: a count, then each as its
  /// name, its LinkImageEntityFlags, its size in bytes, the logarithm of its
  /// alignment and, where the flags say so, 32 bits of a checksum, or the
  /// index of a type_info object's layout shape. Critical.
  LinkImageKept = 1,
  /// The image is an executable, so that an image that imports from it binds
  /// the running executable by name, whatever its file. Empty. Critical.
  LinkImageExecutable = 3,
  /// The splits of vague entities that the image's link acknowledged: a
  /// count, then each as a name, of an entity or of an image after '@'.
  LinkImageAllowed = 4,
  /// The 64 bits of the digest that the image's export of its epoch names
  /// after LinkOffersEpochPrefix, which every DLL that offers an entity and
  /// imports from the image imports.
  LinkImageEpoch = 6,
};

static const char LinkOffersEpochPrefix[] = "__llvm_offers$";

/// An entity's LinkFactVisibilityState is in bits 0 and 1 and a variable's
/// LinkFactInitKind in bits 2 to 4.
enum LinkImageEntityFlags : uint64_t {
  LinkImageEntityInitShift = 2,
  /// No object states the entity: the linker derived that it is one.
  LinkImageEntityForeign = 0x20,
  LinkImageEntityFunction = 0x40,
  /// A constant that an image may read in its own copy, whose bytes the
  /// checksum that follows the entity tells.
  LinkImageEntityCheckSum = 0x80,
  /// A function with which the image replaces an overridable default, which
  /// it exports as the default's name after LinkOverrideOfferPrefix. The
  /// executable of a program takes the first that the images it loads offer,
  /// in breadth-first order.
  LinkImageEntityOverride = 0x100,
  /// Another image's instance may stand for the image's: the image reaches
  /// the entity through a cell that its fill binds to the instance that the
  /// executable's offer table names.
  LinkImageEntityPreemptible = 0x200,
  /// In an offer table: the entry names a word that holds the address of the
  /// instance, not the instance.
  LinkImageEntityIndirect = 0x400,
  /// In an offer table: the entry names an image by its file name, and has
  /// no instance. An executable's names each image that its link sees loaded
  /// with it, and a DLL's the DLL itself.
  LinkImageEntityImage = 0x800,
  /// The DLL's baseline, the import library of a release that the image
  /// replaces, exports no name of the entity: an image linked against that
  /// release may keep its own instance, to which the DLL yields.
  LinkImageEntityAddition = 0x1000,
  /// A type_info object whose name word holds the non-unique bit: its size
  /// is that of the blob that the word names, the name, its NUL and the
  /// layout shape after it, whose bytes the record states. A DLL that keeps
  /// it preemptible binds its name word to the owner's where the blobs are
  /// one.
  LinkImageEntityDescriptor = 0x2000,
  /// What two copies of one entity agree on besides size and alignment:
  /// whether it is a function, whether a guard initializes it and whether
  /// it is a type_info object, whose layout shape is agreed on too. A
  /// function's alignment is 0.
  LinkImageEntityShape = LinkImageEntityFunction |
                         2 << LinkImageEntityInitShift |
                         LinkImageEntityDescriptor,
};

/// An image's offer table, whose RVA the 32-bit word at OfferTableHeaderWord
/// of its DOS header holds: OfferTableMagic, the base-two logarithm of the
/// number of buckets, the number of entries and OfferTableFlags; the entries in
/// the order of their keys, each 24 bytes, the key of the name, the RVA of
/// the NUL-terminated name, the RVA of the instance, the size as 32 bits, the
/// LinkImageEntityFlags as 16 bits, the logarithm of the alignment as 8 bits
/// and a zero byte; then for each bucket, which the top bits of a key index,
/// the index of its first entry as 32 bits, and the number of entries; then
/// the names that the image holds nowhere else. No two names have one key.
enum : uint32_t { OfferTableMagic = 0x4F7A1D2B, OfferTableHeaderWord = 0x38 };
enum OfferTableFlags : uint32_t {
  /// An entry is of a type_info object.
  OfferTableDescriptors = 1,
};

static const char LinkOverrideOfferPrefix[] = "__ovr_offer$";
/// The name, after this, of an overridable default's entry for the calls and
/// jumps of its own image: it checks the default's word, enters the default
/// past its own check where the word holds the default and otherwise jumps
/// through the word.
static const char LinkOverrideOwnPrefix[] = "__ovr_own$";
/// The name, after this, of a DLL's second import of an overridable default
/// whose address it takes, through which its calls of the default go.
static const char LinkOverrideCallPrefix[] = "__ovr_call$";

/// The payload of an Overrides group is the overridable defaults that have a
/// slot and that the DLL or an image loaded with it defines: the value of
/// the type prefix key of the images that define them, which is empty where
/// they have no type prefix, a count and that many defaults in the order of
/// their slots. Each is its name, its slot, one more than the slot of the
/// default that it is specified as calling, or zero, its flags, whose low two
/// bits are 1 where that one takes its arguments without the second, which
/// is the last, and 2 where others follow the second, and whose 4 is set
/// where the default has a type prefix, and then, with 4, the 32-bit type ID
/// that the prefix holds. A name is a size and that many bytes.

/// A program that replaces an overridable default has an override table in
/// the header page of its executable, which the link of the executable
/// writes and the images of the process read once. A slot is two 32-bit
/// words: OverrideTableSlotTag exclusive-or the slot's index, and the RVA of
/// what stands for the slot's default, or zeros where the default stands. The
/// first eight slots are at OverrideTableOffset; then come the header, which
/// is OverrideTableMagic, OverrideTableFormat and the number of slots as
/// 16-bit words, the RVA of the anchor and twelve bytes of zeros; and then
/// the other slots.
///
/// The anchor is the section OverrideAnchorSection, which the image names
/// __ovr_anchor: the magic, the format and the number of slots again, a mask
/// of the slots that a definition replaces rather than one that the slot's
/// default forwards to, the RVA of the image's table of offers, the number
/// of defaults it replaces by their names and a word of zeros, and then the
/// address of what stands for each slot's default, or null.
enum : uint32_t {
  OverrideTableMagic = 0x7B3AC2D5,
  OverrideTableFormat = 1,
  OverrideTableSlotTag = 0xC5E1A7F3,
  OverrideTableOffset = 0x40,
  OverrideTableHeaderOffset = 0x80,
  OverrideTableHeaderSize = 0x18,
  OverrideAnchorHeaderSize = 0x18,
};
static const char OverrideAnchorSection[] = ".ovra";

/// Returns whether a definition named \p Name, in a COMDAT of its own, may be
/// a vague entity. A name of the Microsoft C++ ABI or one that starts with a
/// dot is none, and neither is a virtual table, a VTT, a construction virtual
/// table or a type name, which each image keeps, nor a guard variable or a
/// thread-local wrapper or initialization function, which follow their
/// variables.
LLVM_ABI bool isVagueEntityName(StringRef Name);

/// Writes a link fact whose operands are numbers, a symbol's being its
/// index.
LLVM_ABI void encodeLinkFact(raw_ostream &OS, uint64_t Symbol, uint64_t Tag,
                             ArrayRef<uint64_t> Operands);

enum Arm64ECThunkType : uint8_t {
  GuestExit = 0,
  Entry = 1,
  Exit = 4,
};

inline bool isReservedSectionNumber(int32_t SectionNumber) {
  return SectionNumber <= 0;
}

/// Encode section name based on string table offset.
/// The size of Out must be at least COFF::NameSize.
LLVM_ABI bool encodeSectionName(char *Out, uint64_t Offset);

} // End namespace COFF.
} // End namespace llvm.

#endif
