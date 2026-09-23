//===- CGABIContract.h - Physical ABI requirement emission ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_CODEGEN_CGABICONTRACT_H
#define LLVM_CLANG_LIB_CODEGEN_CGABICONTRACT_H

#include "clang/AST/Type.h"

namespace llvm {
class GlobalObject;
}

namespace clang::CodeGen {
class CodeGenModule;

// Attach physical requirements to the existing identity. Name objects carry
// only their string representation; they do not claim that a type is complete.
void setTypeABIContract(CodeGenModule &CGM, llvm::GlobalObject &Object,
                        QualType Type, bool IsName);
} // namespace clang::CodeGen

#endif
