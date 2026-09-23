//===- OutputFiles.cpp - Final PE output publication ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OutputFiles.h"
#include "COFFLinkerContext.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include <algorithm>

using namespace llvm;

namespace lld::coff {

static std::string normalizedPath(COFFLinkerContext &ctx, StringRef path) {
  SmallString<256> absolute(path);
  if (std::error_code ec = sys::fs::make_absolute(absolute))
    Fatal(ctx) << "cannot resolve output path " << path << ": " << ec.message();
  sys::path::remove_dots(absolute, true);
#ifdef _WIN32
  return StringRef(absolute).lower();
#else
  return absolute.str().str();
#endif
}

OutputFiles::~OutputFiles() {
  discard();
  ctx.e.discardOutputs = nullptr;
}

void OutputFiles::discard() {
  ctx.pendingOutputs.clear();
  for (const File &file : files) {
    if (!file.staging.empty())
      sys::fs::remove(file.staging);
    if (!file.backup.empty())
      sys::fs::remove(file.backup);
  }
  files.clear();
}

std::string OutputFiles::stage(StringRef path) {
  if (!enabled || path == "-")
    return path.str();
  ctx.e.discardOutputs = std::bind(&OutputFiles::discard, this);
  if (!paths.insert(normalizedPath(ctx, path)).second)
    Fatal(ctx) << "multiple outputs use the same path: " << path;
  SmallString<256> temporary;
  if (std::error_code ec =
          sys::fs::createUniqueFile(path + ".tmp-%%%%%%%%", temporary))
    Fatal(ctx) << "cannot stage output " << path << ": " << ec.message();
  files.push_back({path.str(), temporary.str().str(), {}});
  return temporary.str().str();
}

void OutputFiles::rollback() {
  for (File &file : reverse(files)) {
    if (!file.published)
      continue;
    std::error_code ec = file.backup.empty()
                             ? sys::fs::remove(file.path)
                             : sys::fs::rename(file.backup, file.path);
    if (ec) {
      // Preserve a recovery copy if the destination became inaccessible. Do
      // not let context cleanup delete the only remaining original file.
      if (file.backup.empty())
        Warn(ctx) << "cannot remove newly published output " << file.path
                  << ": " << ec.message();
      else
        Warn(ctx) << "cannot restore " << file.path << ": " << ec.message()
                  << "; original retained at " << file.backup;
      file.backup.clear();
    }
    file.published = false;
  }
}

void OutputFiles::publish(StringRef mainPath) {
  if (!enabled)
    return;
  // The main image is the publication point: its forwarders and imports name
  // the exact private members of this set. All encoders have finished before
  // we touch any destination, and ordinary I/O failures restore replacements.
  std::string main = normalizedPath(ctx, mainPath);
  for (size_t i = 0; i != files.size(); ++i)
    if (normalizedPath(ctx, files[i].path) == main) {
      std::rotate(files.begin() + i, files.begin() + i + 1, files.end());
      break;
    }
  for (File &file : files) {
    auto previous = MemoryBuffer::getFile(file.path, false, false);
    if (!previous) {
      if (previous.getError() != errc::no_such_file_or_directory)
        Fatal(ctx) << "cannot preserve output " << file.path << ": "
                   << previous.getError().message();
      continue;
    }
    auto next = MemoryBuffer::getFile(file.staging, false, false);
    if (!next)
      Fatal(ctx) << "cannot read staged output " << file.path << ": "
                 << next.getError().message();
    file.unchanged = (*previous)->getBuffer() == (*next)->getBuffer();
    if (file.unchanged)
      continue;
    previous->reset();
    next->reset();
    SmallString<256> backup;
    if (std::error_code ec =
            sys::fs::createUniqueFile(file.path + ".old-%%%%%%%%", backup))
      Fatal(ctx) << "cannot preserve output " << file.path << ": "
                 << ec.message();
    file.backup = backup.str().str();
    // Hard links preserve the old bytes without copying large PDBs. The
    // replacement below renames a new file over the destination; it never
    // modifies the old inode through this additional name.
    sys::fs::remove(backup);
    if (sys::fs::create_hard_link(file.path, backup))
      if (std::error_code ec = sys::fs::copy_file(file.path, backup))
        Fatal(ctx) << "cannot preserve output " << file.path << ": "
                   << ec.message();
  }
  for (File &file : files) {
    if (file.unchanged)
      continue;
    if (std::error_code ec = sys::fs::rename(file.staging, file.path)) {
      rollback();
      Fatal(ctx) << "cannot publish output " << file.path << ": "
                 << ec.message();
    }
    file.published = true;
  }
  discard();
}

} // namespace lld::coff
