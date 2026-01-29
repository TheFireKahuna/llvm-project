//===-- crt_windows_pseudo_reloc.cpp - Runtime pseudo-relocation support --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runtime pseudo-relocation support for Windows PE/COFF.
//
// When linking with auto-import (-Xlinker -runtime-pseudo-reloc), the linker
// generates pseudo-relocations for data symbol references that cannot be
// resolved at link time (e.g., extern data from DLLs). These relocations must
// be processed at runtime before the program can use those symbols.
//
// This implementation supports both V1 and V2 pseudo-relocation formats.
//
// DESIGN NOTES:
// - This code runs VERY early, before C++ initialization
// - Must not use any CRT functions except for Windows API
// - Memory protection changes are required to patch read-only sections
//
// WHY __builtin_memcpy INSTEAD OF memcpy:
// We use __builtin_memcpy throughout this file for two critical reasons:
// 1. TIMING: This code runs before UCRT is initialized. Calling memcpy()
//    would require UCRT to be ready, which it isn't at pseudo-reloc time.
// 2. STRICT ALIASING: Reading/writing through arbitrary pointers (e.g.,
//    reading a void* from a char* address) violates C++ strict aliasing
//    rules. __builtin_memcpy is the blessed way to do type-punning safely.
// 3. RELIABILITY: __builtin_memcpy is a compiler intrinsic that's always
//    available and generates inline code. It has no external dependencies.
//
// REFERENCES:
// - MinGW-w64 pseudo-reloc.c: The canonical implementation
// - LLD linker: Generates the relocation data
// - https://sourceware.org/binutils/docs/ld/WIN32.html
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "crt_windows_internal.h"

//===----------------------------------------------------------------------===//
// Windows API imports for memory protection
//===----------------------------------------------------------------------===//

extern "C" {
__declspec(dllimport) BOOL __stdcall VirtualProtect(
    LPVOID lpAddress, size_t dwSize, DWORD flNewProtect, DWORD* lpflOldProtect);
__declspec(dllimport) size_t __stdcall VirtualQuery(
    LPCVOID lpAddress, void* lpBuffer, size_t dwLength);
}

// MEMORY_BASIC_INFORMATION structure for VirtualQuery
struct CRT_MEMORY_BASIC_INFORMATION {
  void* BaseAddress;
  void* AllocationBase;
  DWORD AllocationProtect;
  size_t RegionSize;
  DWORD State;
  DWORD Protect;
  DWORD Type;
};

//===----------------------------------------------------------------------===//
// Pseudo-relocation data structures
//
// The linker places pseudo-relocation data between these two symbols.
// The format depends on the version field in the header.
//===----------------------------------------------------------------------===//

// Pseudo-relocation list boundary symbols
//
// The linker places pseudo-relocation data in a special section and generates
// symbols marking the start and end:
//   __RUNTIME_PSEUDO_RELOC_LIST__     - Start of relocation list
//   __RUNTIME_PSEUDO_RELOC_LIST_END__ - End of relocation list
//
// When pseudo-relocations exist, LLD generates these symbols automatically.
// When no pseudo-relocations exist, we provide fallback empty sentinels via
// /alternatename so the code links but processes nothing (start == end).

extern "C" {
// Fallback empty list when linker doesn't generate pseudo-relocations.
// selectany ensures the linker picks one definition if multiple exist.
__declspec(selectany) char __crt_pseudo_reloc_empty__[1] = {0};

// External references - linker provides these when pseudo-relocs exist
extern char __RUNTIME_PSEUDO_RELOC_LIST__[];
extern char __RUNTIME_PSEUDO_RELOC_LIST_END__[];
}

// Use /alternatename to provide fallback if linker doesn't define these symbols.
// This allows programs with no pseudo-relocations to still link.
#pragma comment(linker, "/alternatename:__RUNTIME_PSEUDO_RELOC_LIST__=__crt_pseudo_reloc_empty__")
#pragma comment(linker, "/alternatename:__RUNTIME_PSEUDO_RELOC_LIST_END__=__crt_pseudo_reloc_empty__")

namespace {

// V1 relocation entry (legacy format)
// Relocation: *(base + target) += addend
// Always treats target as 32-bit DWORD
struct PseudoRelocV1 {
  DWORD addend;
  DWORD target;
};

// V2 relocation entry (current format)
// Relocation: *(base + target) += *(base + sym) - (base + sym)
// Supports 8, 16, 32, and 64-bit targets via flags
struct PseudoRelocV2 {
  DWORD sym;
  DWORD target;
  DWORD flags;
};

// V2 header - identifies the relocation format version
// Magic values: 0, 0, version
struct PseudoRelocHeader {
  DWORD magic1;
  DWORD magic2;
  DWORD version;
};

// Flag bits for V2 relocations
constexpr DWORD kRelocFlagSize8 = 8;
constexpr DWORD kRelocFlagSize16 = 16;
constexpr DWORD kRelocFlagSize32 = 32;
constexpr DWORD kRelocFlagSize64 = 64;
constexpr DWORD kRelocFlagSizeMask = 0xFF;

//===----------------------------------------------------------------------===//
// Memory modification tracking
//
// We need to change memory protection to write to read-only sections,
// then restore it afterwards. Track modified pages to restore properly.
//===----------------------------------------------------------------------===//

// Simple fixed-size array for tracking modified sections.
// Most programs have few pseudo-relocs across a small number of pages.
// If this limit is exceeded, we abort with a clear error message.
constexpr int kMaxModifiedSections = 64;

struct ModifiedSection {
  void* address;
  size_t size;
  DWORD oldProtect;
};

ModifiedSection g_modifiedSections[kMaxModifiedSections];
int g_numModifiedSections = 0;

// Determine the appropriate writable protection for a memory region.
// Uses PAGE_READWRITE for data sections, PAGE_EXECUTE_READWRITE for code.
// This minimizes the security impact of making memory writable.
DWORD getWritableProtection(DWORD currentProtect) {
  // Check if the region is executable
  bool isExecutable = (currentProtect & (crt::kPageExecute |
                                          crt::kPageExecuteRead |
                                          crt::kPageExecuteReadwrite)) != 0;

  return isExecutable ? crt::kPageExecuteReadwrite : crt::kPageReadwrite;
}

// Mark a memory region as writable, saving the old protection
// Returns true on success
bool makeWritable(void* addr, size_t size) {
  // Check if this region is already tracked
  for (int i = 0; i < g_numModifiedSections; ++i) {
    char* start = static_cast<char*>(g_modifiedSections[i].address);
    char* end = start + g_modifiedSections[i].size;
    char* newAddr = static_cast<char*>(addr);
    if (newAddr >= start && newAddr + size <= end) {
      // Already covered by an existing entry
      return true;
    }
  }

  if (g_numModifiedSections >= kMaxModifiedSections) {
    // Fatal: too many distinct memory pages need modification.
    // This indicates either a very unusual program layout or a bug.
    // We abort rather than silently failing because silent failure would
    // cause hard-to-diagnose crashes later when unpatched relocations are used.
    crt::fatalErrorEarly("Pseudo-reloc: too many modified sections (limit 64)");
  }

  // Query current protection to determine appropriate writable protection.
  // This minimizes security impact by not making data sections executable.
  CRT_MEMORY_BASIC_INFORMATION mbi;
  DWORD newProtect = crt::kPageReadwrite;  // Default for data

  if (VirtualQuery(addr, &mbi, sizeof(mbi)) >= sizeof(mbi)) {
    newProtect = getWritableProtection(mbi.Protect);
  }

  DWORD oldProtect;
  if (!VirtualProtect(addr, size, newProtect, &oldProtect)) {
    // VirtualProtect failed - this shouldn't happen for valid relocations
    return false;
  }

  g_modifiedSections[g_numModifiedSections].address = addr;
  g_modifiedSections[g_numModifiedSections].size = size;
  g_modifiedSections[g_numModifiedSections].oldProtect = oldProtect;
  ++g_numModifiedSections;

  return true;
}

// Restore all modified sections to their original protection
void restoreProtections() {
  for (int i = 0; i < g_numModifiedSections; ++i) {
    DWORD dummy;
    VirtualProtect(g_modifiedSections[i].address,
                   g_modifiedSections[i].size,
                   g_modifiedSections[i].oldProtect,
                   &dummy);
  }
  g_numModifiedSections = 0;
}

//===----------------------------------------------------------------------===//
// Relocation processing
//===----------------------------------------------------------------------===//

// Get the image base address
// The pseudo-relocation offsets are relative to this base
void* getImageBase() {
  // The image base is stored at the start of the PE header
  // GetModuleHandleW(nullptr) returns the base of the main executable
  return GetModuleHandleW(nullptr);
}

// Process a single V2 relocation
// Returns true on success, false on overflow (relocation too narrow)
bool applyRelocV2(char* base, const PseudoRelocV2* reloc) {
  // Calculate the relocation value:
  // delta = *(base + sym) - (base + sym)
  // This is the difference between the actual address stored at the IAT entry
  // and the IAT entry's own address
  char* symAddr = base + reloc->sym;
  ptrdiff_t delta;

  // Read the pointer stored at symAddr (the actual DLL address)
  void* actualAddr;
  __builtin_memcpy(&actualAddr, symAddr, sizeof(void*));
  delta = reinterpret_cast<char*>(actualAddr) - symAddr;

  // Get the target address and size
  char* targetAddr = base + reloc->target;
  DWORD size = reloc->flags & kRelocFlagSizeMask;

  // Make the target writable
  if (!makeWritable(targetAddr, size / 8)) {
    return false;
  }

  // Apply the relocation based on size
  switch (size) {
  case kRelocFlagSize8: {
    int8_t val;
    __builtin_memcpy(&val, targetAddr, 1);
    val += static_cast<int8_t>(delta);
    __builtin_memcpy(targetAddr, &val, 1);
    break;
  }
  case kRelocFlagSize16: {
    int16_t val;
    __builtin_memcpy(&val, targetAddr, 2);
    val += static_cast<int16_t>(delta);
    __builtin_memcpy(targetAddr, &val, 2);
    break;
  }
  case kRelocFlagSize32: {
    int32_t val;
    __builtin_memcpy(&val, targetAddr, 4);
    // Check for overflow - this is the "too narrow" warning case from the linker
    int64_t newVal = static_cast<int64_t>(val) + delta;
    if (newVal < INT32_MIN || newVal > INT32_MAX) {
      // Overflow! The linker should have warned about this at link time.
      // This is a FATAL error because continuing would cause silent data
      // corruption. The truncated value would be wrong, leading to crashes
      // or security vulnerabilities when the relocation target is accessed.
      //
      // To fix this, either:
      // 1. Use -mcmodel=large or equivalent to generate 64-bit relocations
      // 2. Ensure DLLs are loaded within 2GB of the executable
      // 3. Avoid auto-importing data symbols that require large offsets
      crt::fatalErrorEarly("Pseudo-reloc: 32-bit relocation overflow. "
                           "Address delta exceeds INT32 range.");
    }
    val = static_cast<int32_t>(newVal);
    __builtin_memcpy(targetAddr, &val, 4);
    break;
  }
  case kRelocFlagSize64: {
    int64_t val;
    __builtin_memcpy(&val, targetAddr, 8);
    val += delta;
    __builtin_memcpy(targetAddr, &val, 8);
    break;
  }
  default:
    // Unknown size - skip this relocation
    return false;
  }

  return true;
}

// Process all pseudo-relocations
void doPseudoReloc(char* start, char* end, char* base) {
  if (start >= end) {
    // No relocations
    return;
  }

  // Check for V2 header
  // V2 format starts with: { 0, 0, version }
  // If the first two DWORDs are both 0, this is V2 format
  auto* header = reinterpret_cast<PseudoRelocHeader*>(start);

  if (header->magic1 == 0 && header->magic2 == 0) {
    // V2 format
    DWORD version = header->version;
    if (version != 1) {
      // Unknown version - skip
      return;
    }

    // Skip header, process V2 entries
    auto* reloc = reinterpret_cast<const PseudoRelocV2*>(start + sizeof(PseudoRelocHeader));
    auto* relocEnd = reinterpret_cast<const PseudoRelocV2*>(end);

    while (reloc < relocEnd) {
      applyRelocV2(base, reloc);
      ++reloc;
    }
  } else {
    // V1 format (legacy) - no header, just entries
    auto* reloc = reinterpret_cast<const PseudoRelocV1*>(start);
    auto* relocEnd = reinterpret_cast<const PseudoRelocV1*>(end);

    while (reloc < relocEnd) {
      char* targetAddr = base + reloc->target;

      // V1 always uses 32-bit DWORD
      if (makeWritable(targetAddr, 4)) {
        DWORD val;
        __builtin_memcpy(&val, targetAddr, 4);
        val += reloc->addend;
        __builtin_memcpy(targetAddr, &val, 4);
      }

      ++reloc;
    }
  }

  // Restore memory protections
  restoreProtections();
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

namespace {

// Static initialization - no runtime init needed
INIT_ONCE g_pseudoRelocInitOnce = INIT_ONCE_STATIC_INIT;

// Callback that processes pseudo-relocations
BOOL __stdcall pseudoRelocCallback(PINIT_ONCE, void*, void**) {
  char* base = static_cast<char*>(getImageBase());
  doPseudoReloc(__RUNTIME_PSEUDO_RELOC_LIST__,
                __RUNTIME_PSEUDO_RELOC_LIST_END__,
                base);
  return TRUE;
}

} // namespace

extern "C" {

// Main entry point called by CRT startup
// This function is called before C initializers run
void _pei386_runtime_relocator(void) {
  // InitOnceExecuteOnce guarantees the callback runs exactly once.
  // Any concurrent callers wait efficiently (kernel-assisted, no spin).
  InitOnceExecuteOnce(&g_pseudoRelocInitOnce, pseudoRelocCallback,
                      nullptr, nullptr);
}

} // extern "C"

//===----------------------------------------------------------------------===//
// Internal wrapper for CRT init sequence
//===----------------------------------------------------------------------===//

namespace crt {

void runPseudoRelocator() {
  _pei386_runtime_relocator();
}

} // namespace crt

#endif // _WIN32
