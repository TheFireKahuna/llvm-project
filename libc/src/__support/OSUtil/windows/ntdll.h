//===-- NT API declarations for Windows -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Umbrella header for all NT API declarations. Includes:
// - nt_types.h    -- basic Windows types, NTSTATUS, UNICODE_STRING, etc.
// - nt_memory.h   -- memory constants, structures, and VM API declarations
// - nt_file.h     -- file I/O constants, structures, and API declarations
// - nt_process.h  -- thread, process, and system API declarations
// - nt_context.h  -- exception handling and architecture-specific CONTEXT types
// - nt_security.h -- security, token, and registry declarations
// - nt_job.h      -- job object (process group / resource limit) declarations
// - nt_threadpool.h -- worker factory (kernel thread pool) declarations
// - nt_ioring.h   -- IoRing (io_uring equivalent) declarations
// - nt_afd.h      -- AFD socket ioctl codes and structures
// - nt_error.h    -- NTSTATUS to POSIX errno conversion
// - nt_string.h   -- string conversion, NLS, and Unicode functions
//
// Individual sub-headers can be included directly for narrower dependencies.
//
// This mirrors the Windows architecture where ntdll.dll is the lowest user-mode
// layer, providing syscall stubs that kernel32.dll builds upon.
//
// For ProcessPrng, see bcryptprimitives.h.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NTDLL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NTDLL_H

#include "nt/nt_types.h"
#include "nt/nt_memory.h"
#include "nt/nt_file.h"
#include "nt/nt_process.h"
#include "nt/nt_context.h"
#include "nt/nt_security.h"
#include "nt/nt_job.h"
#include "nt/nt_threadpool.h"
#include "nt/nt_ioring.h"
#include "nt/nt_afd.h"
#include "nt/nt_error.h"
#include "nt/nt_string.h"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NTDLL_H
