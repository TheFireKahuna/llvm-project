//===- OutputFiles.h - Final PE output publication ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_OUTPUTFILES_H
#define LLD_COFF_OUTPUTFILES_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include <string>
#include <vector>

namespace lld::coff {

class COFFLinkerContext;

// Auxiliary writers retain their normal encoders and final, user-visible
// paths. Only their filesystem destination changes. This also covers writers
// such as the PDB builder which commit their own FileOutputBuffer.
class OutputFiles {
public:
  explicit OutputFiles(COFFLinkerContext &ctx) : ctx(ctx) {}
  ~OutputFiles();
  bool enabled = false;
  std::string stage(llvm::StringRef path);
  void publish(llvm::StringRef mainPath);
  void discard();

private:
  struct File {
    std::string path;
    std::string staging;
    std::string backup;
    bool published = false;
    bool unchanged = false;
  };
  void rollback();
  COFFLinkerContext &ctx;
  std::vector<File> files;
  llvm::StringSet<> paths;
};

} // namespace lld::coff

#endif
