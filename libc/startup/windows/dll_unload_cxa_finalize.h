//===-- DLL-unload __cxa_finalize bridge ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single declaration for the cross-TU symbol that veh_core's
// dll_notify_callback invokes on every LDR_DLL_NOTIFICATION_REASON_UNLOADED.
// Defined in libc_init.cpp; called from veh_core.cpp under loader lock.
// Header exists so both sides agree on the signature without
// hand-rolled forward decls drifting.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_STARTUP_WINDOWS_DLL_UNLOAD_CXA_FINALIZE_H
#define LLVM_LIBC_STARTUP_WINDOWS_DLL_UNLOAD_CXA_FINALIZE_H

#include "src/__support/OSUtil/windows/ntdll.h"

extern "C" void __libc_dll_unload_cxa_finalize(
    const LDR_DLL_NOTIFICATION_DATA *data);

#endif // LLVM_LIBC_STARTUP_WINDOWS_DLL_UNLOAD_CXA_FINALIZE_H
