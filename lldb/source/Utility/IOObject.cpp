//===-- IOObject.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Utility/IOObject.h"

#if defined(LLVM_RUNTIME_WIN32)
#include "lldb/Host/windows/windows.h"
#endif

using namespace lldb_private;

#if defined(LLVM_RUNTIME_WIN32)
const IOObject::WaitableHandle IOObject::kInvalidHandleValue =
    INVALID_HANDLE_VALUE;
#else
const IOObject::WaitableHandle IOObject::kInvalidHandleValue = -1;
#endif
IOObject::~IOObject() = default;
