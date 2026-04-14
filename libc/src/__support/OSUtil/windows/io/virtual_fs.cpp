//===-- Virtual special-files namespace -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/io/virtual_fs.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/io/string_utils.h"
#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/libc_assert.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace virtual_fs {

namespace {

struct VirtualDirectoryState {
  NodeKind kind;
};

struct NodeRef {
  NodeKind kind;
  uint32_t pts_id;
};

constexpr char DEV_DIR[] = "/dev";
constexpr char PTS_DIR[] = "/dev/pts";

VirtualDirectoryState DEV_ROOT_STATE{NodeKind::DevRoot};
VirtualDirectoryState PTS_DIR_STATE{NodeKind::PtsDir};

using string_util::streq;

const VirtualDirectoryState *state_from_ofd(
    const OpenFileDescription *ofd) {
  if (!ofd || !ofd->is_virtual_dir())
    return nullptr;
  auto nk = static_cast<NodeKind>(ofd->get_virtual_dir_kind());
  if (nk == NodeKind::DevRoot)
    return &DEV_ROOT_STATE;
  if (nk == NodeKind::PtsDir)
    return &PTS_DIR_STATE;
  return nullptr;
}

bool is_directory_node(NodeKind kind) {
  return kind == NodeKind::DevRoot || kind == NodeKind::PtsDir;
}

bool is_digit_string(const char *text, uint32_t *value_out) {
  if (!text || *text == '\0')
    return false;
  uint32_t value = 0;
  while (*text) {
    if (*text < '0' || *text > '9')
      return false;
    // Overflow guard: check before multiply to catch values > UINT32_MAX.
    if (value > UINT32_MAX / 10)
      return false;
    uint32_t next = value * 10 + static_cast<uint32_t>(*text - '0');
    if (next < value)
      return false;
    value = next;
    ++text;
  }
  if (value_out)
    *value_out = value;
  return true;
}

bool next_component(const char **cursor, const char **start,
                                size_t *len) {
  const char *p = *cursor;
  while (*p == '/')
    ++p;
  if (*p == '\0') {
    *cursor = p;
    return false;
  }
  *start = p;
  while (*p != '\0' && *p != '/')
    ++p;
  *len = static_cast<size_t>(p - *start);
  *cursor = p;
  return true;
}

bool is_component(const char *start, size_t len,
                              const char *literal) {
  size_t i = 0;
  for (; i < len && literal[i] != '\0'; ++i) {
    if (start[i] != literal[i])
      return false;
  }
  return i == len && literal[i] == '\0';
}

NodeRef invalid_node() { return {NodeKind::Invalid, 0}; }

NodeRef resolve_relative(NodeKind base, const char *path) {
  const char *cursor = path;
  NodeKind current = base;
  uint32_t pts_id = 0;

  const char *component = nullptr;
  size_t len = 0;
  while (next_component(&cursor, &component, &len)) {
    if (is_component(component, len, "."))
      continue;

    if (is_component(component, len, "..")) {
      if (current == NodeKind::PtsDir)
        current = NodeKind::DevRoot;
      else if (current != NodeKind::DevRoot)
        return invalid_node();
      continue;
    }

    switch (current) {
    case NodeKind::DevRoot:
      if (is_component(component, len, "pts")) {
        current = NodeKind::PtsDir;
      } else if (is_component(component, len, "ptmx")) {
        current = NodeKind::Ptmx;
      } else {
        return invalid_node();
      }
      break;
    case NodeKind::PtsDir: {
      uint32_t id = 0;
      if (!is_digit_string(component, &id))
        return invalid_node();
      current = NodeKind::PtsSlave;
      pts_id = id;
      break;
    }
    default:
      return invalid_node();
    }
  }

  return {current, pts_id};
}

NodeRef resolve_absolute(const char *path) {
  if (!path || path[0] != '/')
    return invalid_node();
  // Bare "/" is not a virtual-fs node — only /dev and below are handled.
  if (path[1] == '\0')
    return invalid_node();
  if (streq(path, DEV_DIR) || streq(path, "/dev/"))
    return {NodeKind::DevRoot, 0};
  if (streq(path, PTS_DIR) || streq(path, "/dev/pts/"))
    return {NodeKind::PtsDir, 0};

  const char *cursor = path + 1;
  const char *component = nullptr;
  size_t len = 0;
  if (!next_component(&cursor, &component, &len))
    return invalid_node();
  if (!is_component(component, len, "dev"))
    return invalid_node();
  return resolve_relative(NodeKind::DevRoot, cursor);
}

NodeRef resolve_node(int dirfd, const char *path) {
  if (!path || path[0] == '\0')
    return invalid_node();
  if (path[0] == '/')
    return resolve_absolute(path);

  OpenFileDescription *ofd = fd_table.get_ofd(dirfd);
  const VirtualDirectoryState *state = state_from_ofd(ofd);
  if (!state)
    return invalid_node();
  return resolve_relative(state->kind, path);
}

bool has_trailing_slash(const char *path) {
  if (!path)
    return false;
  const char *p = path;
  const char *last = nullptr;
  while (*p) {
    last = p;
    ++p;
  }
  return last && *last == '/';
}

ErrorOr<int> open_virtual_dir(NodeKind kind, int flags) {
  int accmode = flags & O_ACCMODE;
  if (accmode == O_WRONLY || accmode == O_RDWR)
    return Error(EISDIR);
  if (flags & (O_CREAT | O_EXCL | O_TRUNC))
    return Error(EINVAL);

  int normalized = (flags & (O_CLOEXEC | O_NONBLOCK | O_PATH)) | O_RDONLY;
  // Pass dir_kind as aux_byte so it's set before the fd slot is published,
  // avoiding a TOCTOU where a concurrent thread sees an uninitialized kind.
  auto result = fd_table.alloc_synthetic(FileKind::VirtualDir, normalized, 0,
                                         static_cast<uint8_t>(kind));
  if (result.has_value() && (flags & O_CLOEXEC))
    fd_table.set_fd_cloexec(result.value(), true);
  return result;
}

ErrorOr<int> open_node(NodeRef node, const char *path, int flags) {
  if (node.kind == NodeKind::Invalid)
    return Error(ENOENT);

  bool trailing_slash = has_trailing_slash(path);
  if (trailing_slash && !is_directory_node(node.kind))
    return Error(ENOTDIR);

  switch (node.kind) {
  case NodeKind::DevRoot:
  case NodeKind::PtsDir:
    return open_virtual_dir(node.kind, flags);
  case NodeKind::Ptmx:
    if (flags & O_DIRECTORY)
      return Error(ENOTDIR);
    return vt_pty::posix_openpt(flags);
  case NodeKind::PtsSlave:
    if (flags & O_DIRECTORY)
      return Error(ENOTDIR);
    return vt_pty::open_pts_id(node.pts_id, flags);
  default:
    return Error(ENOENT);
  }
}

} // namespace

ErrorOr<int> openat(int dirfd, const char *path, int flags) {
  return open_node(resolve_node(dirfd, path), path, flags);
}

bool is_virtual_dir(const OpenFileDescription *ofd) {
  return ofd && ofd->is_virtual_dir() && state_from_ofd(ofd);
}

NodeKind directory_kind(const OpenFileDescription *ofd) {
  const VirtualDirectoryState *state = state_from_ofd(ofd);
  return state ? state->kind : NodeKind::Invalid;
}

void release_opaque(void *) {}

} // namespace virtual_fs
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
