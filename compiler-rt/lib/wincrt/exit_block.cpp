//===-- exit_block.cpp - The executable's exit-list state ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every executable reserves the process's exit-list state, which every copy of
// the termination registries finds through the locator the linker writes for
// __wincrt_exit_state in the executable's MZ header (exit_state.h). An
// executable's entry point links this object.
//
//===----------------------------------------------------------------------===//

#include "exit_state.h"

extern "C" wincrt::ExitState __wincrt_exit_state = {wincrt::ExitStateVersion,
                                                    sizeof(wincrt::ExitState)};
