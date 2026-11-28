//===-- WindowsMiniDump.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// This function is separated out from ObjectFilePECOFF.cpp to name avoid name
// collisions with WinAPI preprocessor macros.

#include "WindowsMiniDump.h"
#include "lldb/Utility/FileSpec.h"

#ifdef LLVM_RUNTIME_WIN32
#include "lldb/Host/windows/windows.h"
#include "llvm/Support/Windows/WindowsSupport.h"
#include <dbghelp.h>
#endif

namespace lldb_private {

bool SaveMiniDump(const lldb::ProcessSP &process_sp,
                  SaveCoreOptions &core_options, lldb_private::Status &error) {
  if (!process_sp)
    return false;
#ifdef LLVM_RUNTIME_WIN32
  std::optional<FileSpec> outfileSpec = core_options.GetOutputFile();
  const auto &outfile = outfileSpec.value();
  HANDLE process_handle = ::OpenProcess(
      PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, process_sp->GetID());
  llvm::SmallVector<wchar_t, MAX_PATH> wide_name;
  if (llvm::sys::windows::widenPath(outfile.GetPath(), wide_name)) {
    error = Status::FromErrorString("cannot convert file name");
    return false;
  }
  HANDLE file_handle =
      ::CreateFileW(wide_name.data(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  const auto result =
      ::MiniDumpWriteDump(process_handle, process_sp->GetID(), file_handle,
                          MiniDumpWithFullMemoryInfo, NULL, NULL, NULL);
  ::CloseHandle(file_handle);
  ::CloseHandle(process_handle);
  if (!result) {
    error = Status(::GetLastError(), lldb::eErrorTypeWin32);
    return false;
  }
  return true;
#endif
  return false;
}

} // namesapce lldb_private
