//===-- x64 unwind code types for .pdata/.xdata parsing ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Structures for parsing the x64 table-driven unwind metadata emitted by
// the compiler into PE .pdata (RUNTIME_FUNCTION[]) and .xdata (UNWIND_INFO
// + UNWIND_CODE[]) sections.
//
// Used by the custom async-signal-safe stack walker in debug/stack_walker.cpp.
// RUNTIME_FUNCTION itself is defined in <sys/ntabi.h>.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_UNWIND_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_UNWIND_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#ifdef __x86_64__

namespace LIBC_NAMESPACE_DECL {
namespace unwind {

// -----------------------------------------------------------------------
// UWOP — unwind operation codes (x64)
// -----------------------------------------------------------------------

enum Opcode : unsigned char {
  kPushNonVol = 0,     // 1 slot.  info = register number
  kAllocLarge = 1,     // 2-3 slots. info=0: next u16*8, info=1: next u32
  kAllocSmall = 2,     // 1 slot.  size = info * 8 + 8
  kSetFpReg = 3,       // 1 slot.  uses UnwindInfo.FrameRegister/Offset
  kSaveNonVol = 4,     // 2 slots. info = reg, next u16 = offset/8
  kSaveNonVolFar = 5,  // 3 slots. info = reg, next u32 = offset
  kEpilog = 6,         // 1-2 slots (v2 only, skip during walk)
  kSpareCode = 7,      // unused
  kSaveXmm128 = 8,     // 2 slots. skip (XMM irrelevant for stack walk)
  kSaveXmm128Far = 9,  // 3 slots. skip
  kPushMachFrame = 10, // 1 slot.  info=0: no error code, info=1: error code
};

// Number of UNWIND_CODE slots consumed by a given opcode.
LIBC_INLINE int slots_for(unsigned char opcode, unsigned char info) {
  switch (opcode) {
  case kPushNonVol:    return 1;
  case kAllocLarge:    return info == 0 ? 2 : 3;
  case kAllocSmall:    return 1;
  case kSetFpReg:      return 1;
  case kSaveNonVol:    return 2;
  case kSaveNonVolFar: return 3;
  case kEpilog:        return 1;
  case kSpareCode:     return 1;
  case kSaveXmm128:    return 2;
  case kSaveXmm128Far: return 3;
  case kPushMachFrame: return 1;
  default:             return 1;
  }
}

// -----------------------------------------------------------------------
// UNWIND_CODE — 2-byte entry in the unwind code array
// -----------------------------------------------------------------------

struct Code {
  unsigned char offset;      // Prolog byte offset where this op completes.
  unsigned char op_and_info; // Low 4 bits = Opcode, high 4 bits = info.

  LIBC_INLINE unsigned char opcode() const { return op_and_info & 0x0F; }
  LIBC_INLINE unsigned char info() const { return op_and_info >> 4; }

  LIBC_INLINE USHORT next_u16() const {
    return reinterpret_cast<const USHORT *>(this)[1];
  }
  LIBC_INLINE ULONG next_u32() const {
    auto *p = reinterpret_cast<const USHORT *>(this);
    return static_cast<ULONG>(p[1]) | (static_cast<ULONG>(p[2]) << 16);
  }
};

static_assert(sizeof(Code) == 2, "UNWIND_CODE must be 2 bytes");

// -----------------------------------------------------------------------
// UNWIND_INFO — 4-byte header followed by Code[code_count]
// -----------------------------------------------------------------------

struct Info {
  unsigned char ver_flags;     // Version:3 (low) | Flags:5 (high)
  unsigned char prolog_size;   // Size of the function prolog in bytes.
  unsigned char code_count;    // Number of UNWIND_CODE entries.
  unsigned char frame_reg_off; // FrameRegister:4 (low) | FrameOffset:4 (high)

  LIBC_INLINE unsigned char version() const { return ver_flags & 0x07; }
  LIBC_INLINE unsigned char flags() const { return ver_flags >> 3; }
  LIBC_INLINE unsigned char frame_register() const {
    return frame_reg_off & 0x0F;
  }
  // Returns FrameOffset already scaled by 16 (high nibble, low nibble zeroed).
  LIBC_INLINE unsigned char frame_offset_scaled() const {
    return frame_reg_off & 0xF0;
  }

  LIBC_INLINE const Code *codes() const {
    return reinterpret_cast<const Code *>(this + 1);
  }

  // Chained RUNTIME_FUNCTION, present when flags() & UNW_FLAG_CHAININFO.
  // Located after the codes array, padded to an even slot count.
  LIBC_INLINE const RUNTIME_FUNCTION *chained_entry() const {
    unsigned padded = (code_count + 1) & ~1u;
    return reinterpret_cast<const RUNTIME_FUNCTION *>(codes() + padded);
  }
};

static_assert(sizeof(Info) == 4, "UNWIND_INFO header must be 4 bytes");

inline constexpr int MAX_CHAIN_DEPTH = 32;
inline constexpr int GPR_COUNT = 16;

} // namespace unwind
} // namespace LIBC_NAMESPACE_DECL

#endif // __x86_64__

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_UNWIND_TYPES_H
