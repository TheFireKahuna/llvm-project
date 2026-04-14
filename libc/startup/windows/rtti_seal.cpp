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
//   4. Unmap the original .rdata$ti pages
//   5. Map the SEC_NO_CHANGE section R/O at the original VA
//   6. Close the temporary R/W view and section handle
//
// After step 5, NtProtectVirtualMemory on the sealed pages returns
// STATUS_INVALID_PAGE_PROTECTION — they are irrevocably read-only.
// Under HVCI, kernel-mode protection changes are also blocked.
//
// The compiler emits typeinfo into .rdata$ti with dynamic init functions
// registered in @llvm.global_ctors (priority 200, maps to .CRT$XIB).
// This module's seal function runs from .CRT$XIY — after all init ctors
// but before C++ constructors (.CRT$XC*).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"

// .rdata$ti section sentinels — the linker sorts alphabetically, so
// .rdata$tia comes before any .rdata$ti[b-x] and .rdata$tiz comes after.
#pragma section(".rdata$tia", read, write)
#pragma section(".rdata$tiz", read, write)

extern "C" {
__declspec(allocate(".rdata$tia")) __declspec(selectany)
    char __rtti_section_start = 0;
__declspec(allocate(".rdata$tiz")) __declspec(selectany)
    char __rtti_section_end = 0;
}

namespace {

int __cdecl seal_rtti_section() {
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

  // 4. Destroy the temporary R/W view.
  ::NtUnmapViewOfSectionEx(NtCurrentProcess(), rw_base, /*Flags=*/0);

  // 5. Unmap the original .rdata$ti pages so we can replace them.
  ::NtUnmapViewOfSectionEx(NtCurrentProcess(), start, /*Flags=*/0);

  // 6. Map the SEC_NO_CHANGE section R/O at the original address.
  //    After this, NtProtectVirtualMemory will reject any attempt to
  //    escalate protection — the pages are irrevocably read-only.
  PVOID ro_base = static_cast<PVOID>(start);
  SIZE_T ro_size = 0;
  LARGE_INTEGER ro_offset = {};
  status = ::NtMapViewOfSectionEx(
      section, NtCurrentProcess(), &ro_base, &ro_offset, &ro_size,
      /*AllocationType=*/0, PAGE_READONLY,
      /*ExtendedParameters=*/nullptr, /*ExtendedParameterCount=*/0);
  ::NtClose(section);

  if (!NT_SUCCESS(status))
    return -1;

  return 0;
}

} // namespace

// Register as .CRT$XIY — runs after all .CRT$XIB (typeinfo init) entries
// but before .CRT$XC* (C++ constructors). This guarantees all RTTI is
// initialized and sealed before any user code executes.
using PIFV = int(__cdecl *)(void);

#pragma section(".CRT$XIY", long, read)
__declspec(allocate(".CRT$XIY")) __declspec(selectany)
    PIFV __rtti_seal_init = seal_rtti_section;

// Force the linker to pull in this object when RTTI sealing is needed.
#pragma comment(linker, "/include:__rtti_seal_init")
