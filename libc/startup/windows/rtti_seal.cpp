//===-- RTTI section sealing for Windows Itanium/NTPOSIX ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Seals the .rdata$ti section (dynamically-initialized typeinfo objects) after
// all RTTI init constructors have run. Uses a SEC_NO_CHANGE pagefile-backed
// section to make protection irrevocable:
//
//   1. Create pagefile section with SEC_COMMIT | SEC_NO_CHANGE
//   2. Map R/W view at kernel-chosen address (temporary)
//   3. Copy initialized typeinfo data into it
//   4. Probe-map the section PAGE_READONLY at a kernel-chosen address to
//      validate that the section is mappable RO. If this fails, the
//      original .rdata$ti is still intact and we abort cleanly.
//   5. Unmap the temporary R/W view and the probe view
//   6. Unmap the original .rdata$ti pages (destructive — original VA gone)
//   7. Map the SEC_NO_CHANGE section R/O at the original VA. The probe in
//      step 4 already proved the kernel will accept this mapping.
//   8. Close the section handle
//
// The probe in step 4 closes the historical hole where step 6's destructive
// unmap could be followed by a step-7 failure leaving .rdata$ti permanently
// unmapped — exception type-info reads would then trap with no recovery.
//
// After step 7, NtProtectVirtualMemory on the sealed pages returns
// STATUS_INVALID_PAGE_PROTECTION — they are irrevocably read-only.
// Under HVCI, kernel-mode protection changes are also blocked.
//
// The compiler emits typeinfo into .rdata$ti with dynamic init functions
// registered in @llvm.global_ctors (priority 200, maps to .CRT$XIB).
// This module's seal function runs from .CRT$XIY — after all init ctors
// but before C++ constructors (.CRT$XC*).
//
//===----------------------------------------------------------------------===//

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"

// .rdata$ti section sentinels — the linker sorts alphabetically, so
// .rdata$tia comes before any .rdata$ti[b-x] and .rdata$tiz comes after.
// The bookend `char = 0` variables are non-const, so clang registers
// .rdata$tia/.rdata$tiz as R/W — matching the compiler-emitted typeinfo
// objects in between, which have dynamic-init ctors.
extern "C" {
__LIBC_SECTION_ATTR(".rdata$tia") __LIBC_SELECTANY_ATTR
    char __rtti_section_start = 0;
__LIBC_SECTION_ATTR(".rdata$tiz") __LIBC_SELECTANY_ATTR
    char __rtti_section_end = 0;
}

namespace {

int seal_rtti_section() {
  auto *start = reinterpret_cast<char *>(&__rtti_section_start);
  auto *end = reinterpret_cast<char *>(&__rtti_section_end);

  // Nothing to seal if the section is empty (no dynamically-initialized RTTI).
  if (start >= end)
    return 0;

  auto size = static_cast<SIZE_T>(end - start);

  // 1. Create a pagefile-backed section with SEC_NO_CHANGE.
  //    Views mapped from this section cannot have their protection
  //    escalated beyond what was specified at map time.
  HANDLE section = nullptr;
  LARGE_INTEGER section_size;
  section_size.QuadPart = static_cast<LONGLONG>(size);
  NTSTATUS status = ::NtCreateSectionEx(
      &section,
      SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
      /*ObjectAttributes=*/nullptr, &section_size, PAGE_READWRITE,
      SEC_COMMIT | SEC_NO_CHANGE, /*FileHandle=*/nullptr,
      /*ExtendedParameters=*/nullptr, /*ExtendedParameterCount=*/0);
  if (!NT_SUCCESS(status))
    return -1;

  // 2. Map a temporary R/W view at a kernel-chosen address.
  PVOID rw_base = nullptr;
  SIZE_T rw_size = 0;
  LARGE_INTEGER rw_offset = {};
  status = ::NtMapViewOfSectionEx(
      section, NtCurrentProcess(), &rw_base, &rw_offset, &rw_size,
      /*AllocationType=*/0, PAGE_READWRITE,
      /*ExtendedParameters=*/nullptr, /*ExtendedParameterCount=*/0);
  if (!NT_SUCCESS(status)) {
    ::NtClose(section);
    return -1;
  }

  // 3. Copy the initialized typeinfo data (filled by .CRT$XIB init ctors).
  __builtin_memcpy(rw_base, start, size);

  // 4. Probe map: verify the section can be mapped PAGE_READONLY before we
  //    destroy the original .rdata$ti pages. If the kernel will reject the
  //    real mapping for any reason (resource pressure, policy), we want to
  //    discover it now while the original mapping is still intact.
  PVOID probe_base = nullptr;
  SIZE_T probe_size = 0;
  LARGE_INTEGER probe_offset = {};
  status = ::NtMapViewOfSectionEx(
      section, NtCurrentProcess(), &probe_base, &probe_offset, &probe_size,
      /*AllocationType=*/0, PAGE_READONLY,
      /*ExtendedParameters=*/nullptr, /*ExtendedParameterCount=*/0);
  if (!NT_SUCCESS(status)) {
    ::NtUnmapViewOfSectionEx(NtCurrentProcess(), rw_base, /*Flags=*/0);
    ::NtClose(section);
    return -1;
  }

  // 5. Tear down both temporary views — we proved the section maps RO.
  ::NtUnmapViewOfSectionEx(NtCurrentProcess(), probe_base, /*Flags=*/0);
  ::NtUnmapViewOfSectionEx(NtCurrentProcess(), rw_base, /*Flags=*/0);

  // 6. Unmap the original .rdata$ti pages. This is the destructive step —
  //    from here on the original VA is gone until step 7 succeeds. The
  //    probe in step 4 has already validated that step 7 will be accepted.
  ::NtUnmapViewOfSectionEx(NtCurrentProcess(), start, /*Flags=*/0);

  // 7. Map the SEC_NO_CHANGE section R/O at the original address.
  //    After this, NtProtectVirtualMemory will reject any attempt to
  //    escalate protection — the pages are irrevocably read-only.
  PVOID ro_base = static_cast<PVOID>(start);
  SIZE_T ro_size = 0;
  LARGE_INTEGER ro_offset = {};
  status = ::NtMapViewOfSectionEx(
      section, NtCurrentProcess(), &ro_base, &ro_offset, &ro_size,
      /*AllocationType=*/0, PAGE_READONLY,
      /*ExtendedParameters=*/nullptr, /*ExtendedParameterCount=*/0);
  // 8. Close the section handle.
  ::NtClose(section);

  if (!NT_SUCCESS(status)) {
    // Step 7 failure is unrecoverable — original .rdata$ti is gone. The
    // probe in step 4 makes this path nearly impossible (only a TOCTOU
    // race against an external mapper at the original VA could trigger
    // it), but if it does happen, fail loudly via int 0x29 rather than
    // returning -1 and letting the process limp along with unmapped RTTI.
    __asm__ volatile("int $0x29" : : "c"(7) : "memory"); // fastfail
    __builtin_unreachable();
  }

  return 0;
}

} // namespace

// Register as .CRT$XIY — runs after all .CRT$XIB (typeinfo init) entries
// but before .CRT$XC* (C++ constructors). This guarantees all RTTI is
// initialized and sealed before any user code executes.
using PIFV = int(*)(void);

// extern const → clang registers .CRT$XIY as PSF_Read, no #pragma needed.
// `[[gnu::retain]]` emits /INCLUDE: into `.drectve` so the RTTI seal
// initializer is always pulled in.
[[gnu::retain]] __LIBC_SECTION_ATTR(".CRT$XIY") __LIBC_SELECTANY_ATTR extern const
    PIFV __rtti_seal_init = seal_rtti_section;
