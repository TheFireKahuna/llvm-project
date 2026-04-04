//===-- SectionRegistry runtime helpers -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// verify_section_readonly() — walk the PE section headers of the image
// containing `in_section_addr`, locate the section, and confirm
// IMAGE_SCN_MEM_READ set + IMAGE_SCN_MEM_WRITE clear. Called at Tier B
// init to catch a downstream `#pragma section(".<name>$M", read, write)`
// that would silently union MEM_WRITE into the output section and defeat
// the read-only posture promised by LIBC_DECLARE_SECTION_REGISTRY.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/section_registry.h"

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

bool verify_section_readonly(const void *image_base,
                             const void *in_section_addr) {
  if (image_base == nullptr || in_section_addr == nullptr)
    return false;

  auto base = reinterpret_cast<const unsigned char *>(image_base);
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;

  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return false;

  // The section table immediately follows the OptionalHeader. Its length
  // is given by SizeOfOptionalHeader (not sizeof(IMAGE_OPTIONAL_HEADER64))
  // so we tolerate future optional-header growth.
  auto section = reinterpret_cast<const IMAGE_SECTION_HEADER *>(
      reinterpret_cast<const unsigned char *>(&nt->OptionalHeader) +
      nt->FileHeader.SizeOfOptionalHeader);

  auto target_rva = static_cast<ULONG>(
      reinterpret_cast<const unsigned char *>(in_section_addr) - base);

  USHORT count = nt->FileHeader.NumberOfSections;
  for (USHORT i = 0; i < count; ++i) {
    ULONG start = section[i].VirtualAddress;
    ULONG size = section[i].Misc.VirtualSize;
    if (target_rva >= start && target_rva < start + size) {
      ULONG ch = section[i].Characteristics;
      return (ch & IMAGE_SCN_MEM_READ) != 0 &&
             (ch & IMAGE_SCN_MEM_WRITE) == 0;
    }
  }

  // Address not found in any section — the caller handed us a probe
  // pointer outside the image. Treat as failure.
  return false;
}

// STATUS_ENTRYPOINT_NOT_FOUND is repurposed as the bugcheck code for a
// section-RO contract violation: the merged output section that
// LIBC_DECLARE_SECTION_REGISTRY promised would be PAGE_READONLY came back
// IMAGE_SCN_MEM_WRITE. The bugcheck must be release-mode unconditional —
// a writable dispatch table is a CFG-bypass primitive, and shipping with
// LIBC_ASSERT-only enforcement makes the contract a debug-build-only
// guarantee.
[[noreturn]] void fail_section_writable(const char * /*section_name*/) {
  // STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139). Visible in WER reports and
  // dump banners; chosen because it is unique to a static-link integrity
  // class of failure and so will not be confused with a generic Tier B
  // init failure (0xC0000142, STATUS_DLL_INIT_FAILED).
  ::NtTerminateProcess(NtCurrentProcess(),
                       static_cast<NTSTATUS>(0xC0000139L));
  __builtin_unreachable();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
