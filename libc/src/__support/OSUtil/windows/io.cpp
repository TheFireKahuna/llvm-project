//===------------- Windows implementation of IO utils -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "io.h"
#include "src/__support/OSUtil/windows/io/read_write.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

void write_to_stderr(cpp::string_view msg) {
  // Try the FD-table path first — covers redirected stderr after Tier A.
  if (internal::write(2, msg.data(), msg.size()) >= 0)
    return;
  // Fallback: NtWriteFile directly on the PEB-resident stderr handle.
  // The loader always populates ProcessParameters->StandardError, so this
  // works during early Tier A init (before fd_table comes online) — the
  // window in which LIBC_ASSERT failures would otherwise be silent.
  HANDLE h = NtCurrentStandardError();
  if (!h)
    return;
  IO_STATUS_BLOCK iosb{};
  ::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb,
                const_cast<char *>(msg.data()),
                static_cast<ULONG>(msg.size()), nullptr, nullptr);
}

} // namespace LIBC_NAMESPACE_DECL
