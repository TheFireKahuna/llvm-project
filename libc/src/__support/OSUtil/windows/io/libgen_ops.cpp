//===-- Windows lexical libgen helpers -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// basename() / dirname() are specified by POSIX as lexical path splitters.
// This implementation uses the POSIX Issue 8 rules as the contract, musl as a
// behavioral reference for the slash-only core, and extends the parser to
// treat Windows separator and root forms as first-class path syntax:
//
//   - '/' and '\' are both recognized as separators
//   - exact '//' and '\\' roots are preserved
//   - drive roots such as 'C:\' are kept intact
//   - UNC roots such as '\\server\share' are kept intact
//   - '\\?\', '\\.\', and '\??\' namespace prefixes are parsed lexically
//
// The code never performs path normalization or resolution beyond identifying
// lexical root prefixes, which keeps dirname() compliant with POSIX.
//
// Policy note: this implementation intentionally accepts Windows-native path
// syntax in addition to POSIX '/' paths. If the libc grows a future
// strict-POSIX mode, the separator/root recognition should become policy-based
// so basename()/dirname() can be restricted to slash-only semantics without
// rewriting the lexical split logic.
//
//===----------------------------------------------------------------------===//

#include "libgen_ops.h"

#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace {

using cpp::string_view;

enum class RootKind {
  NONE,
  SINGLE_SEPARATOR,
  DOUBLE_SEPARATOR,
  DRIVE_ABSOLUTE,
  UNC,
  NAMESPACE_PREFIX,
};

struct RootInfo {
  RootKind kind;
  size_t length;
  char separator;
};

constexpr bool is_separator(char c) {
  return c == '/' || c == '\\';
}

constexpr bool is_drive_letter(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

constexpr char ascii_upper(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

constexpr bool ascii_ieq(char lhs, char rhs) {
  return ascii_upper(lhs) == ascii_upper(rhs);
}

size_t scan_component(string_view path, size_t start) {
  while (start < path.size() && !is_separator(path[start]))
    ++start;
  return start;
}

size_t scan_unc_root(string_view path, size_t start) {
  if (start >= path.size() || is_separator(path[start]))
    return 0;

  size_t server_end = scan_component(path, start);
  if (server_end >= path.size() || !is_separator(path[server_end]))
    return 0;

  size_t share_start = server_end + 1;
  if (share_start >= path.size() || is_separator(path[share_start]))
    return 0;

  return scan_component(path, share_start);
}

bool has_unc_tag(string_view path, size_t start) {
  return start + 3 < path.size() && ascii_ieq(path[start], 'U') &&
         ascii_ieq(path[start + 1], 'N') &&
         ascii_ieq(path[start + 2], 'C') && is_separator(path[start + 3]);
}

RootInfo detect_root(string_view path) {
  if (path.empty())
    return {RootKind::NONE, 0, '/'};

  if (path.size() >= 4 && is_separator(path[0]) && is_separator(path[1]) &&
      (path[2] == '?' || path[2] == '.') && is_separator(path[3])) {
    constexpr size_t PREFIX_LEN = 4;
    if (path.size() >= PREFIX_LEN + 3 && is_drive_letter(path[PREFIX_LEN]) &&
        path[PREFIX_LEN + 1] == ':' && is_separator(path[PREFIX_LEN + 2])) {
      return {RootKind::NAMESPACE_PREFIX, PREFIX_LEN + 3, path[3]};
    }

    if (has_unc_tag(path, PREFIX_LEN)) {
      if (size_t unc_len = scan_unc_root(path, PREFIX_LEN + 4))
        return {RootKind::NAMESPACE_PREFIX, unc_len, path[3]};
    }

    return {RootKind::NAMESPACE_PREFIX, PREFIX_LEN, path[3]};
  }

  if (path.size() >= 4 && is_separator(path[0]) && path[1] == '?' &&
      path[2] == '?' && is_separator(path[3])) {
    constexpr size_t PREFIX_LEN = 4;
    if (path.size() >= PREFIX_LEN + 3 && is_drive_letter(path[PREFIX_LEN]) &&
        path[PREFIX_LEN + 1] == ':' && is_separator(path[PREFIX_LEN + 2])) {
      return {RootKind::NAMESPACE_PREFIX, PREFIX_LEN + 3, path[0]};
    }

    if (has_unc_tag(path, PREFIX_LEN)) {
      if (size_t unc_len = scan_unc_root(path, PREFIX_LEN + 4))
        return {RootKind::NAMESPACE_PREFIX, unc_len, path[0]};
    }

    return {RootKind::NAMESPACE_PREFIX, PREFIX_LEN, path[0]};
  }

  if (path.size() >= 3 && is_drive_letter(path[0]) && path[1] == ':' &&
      is_separator(path[2])) {
    return {RootKind::DRIVE_ABSOLUTE, 3, path[2]};
  }

  if (path.size() >= 2 && is_separator(path[0]) && is_separator(path[1])) {
    if (path.size() >= 3 && is_separator(path[2]))
      return {RootKind::SINGLE_SEPARATOR, 1, path[0]};

    if (size_t unc_len = scan_unc_root(path, 2))
      return {RootKind::UNC, unc_len, path[0]};

    return {RootKind::DOUBLE_SEPARATOR, 2, path[0]};
  }

  if (is_separator(path[0]))
    return {RootKind::SINGLE_SEPARATOR, 1, path[0]};

  return {RootKind::NONE, 0, '/'};
}

char *dot_literal() {
  static constexpr char DOT[] = ".";
  return const_cast<char *>(DOT);
}

char *root_literal(const RootInfo &root) {
  static constexpr char SLASH[] = "/";
  static constexpr char DOUBLE_SLASH[] = "//";
  static constexpr char BACKSLASH[] = "\\";
  static constexpr char DOUBLE_BACKSLASH[] = "\\\\";

  if (root.kind == RootKind::DOUBLE_SEPARATOR) {
    return const_cast<char *>(root.separator == '\\' ? DOUBLE_BACKSLASH
                                                     : DOUBLE_SLASH);
  }

  return const_cast<char *>(root.separator == '\\' ? BACKSLASH : SLASH);
}

char *truncate_and_return(char *path, size_t length) {
  path[length] = '\0';
  return path;
}

char *root_result(char *path, const RootInfo &root) {
  if (root.kind == RootKind::SINGLE_SEPARATOR ||
      root.kind == RootKind::DOUBLE_SEPARATOR) {
    return root_literal(root);
  }
  return truncate_and_return(path, root.length);
}

} // namespace

char *basename(char *path) {
  if (path == nullptr || path[0] == '\0')
    return dot_literal();

  string_view view(path);
  RootInfo root = detect_root(view);
  size_t end = view.size();

  while (end > root.length && is_separator(path[end - 1])) {
    path[end - 1] = '\0';
    --end;
  }

  if (end == root.length && root.kind != RootKind::NONE)
    return root_result(path, root);

  size_t start = end;
  while (start > root.length && !is_separator(path[start - 1]))
    --start;

  return path + start;
}

char *dirname(char *path) {
  if (path == nullptr || path[0] == '\0')
    return dot_literal();

  string_view view(path);
  RootInfo root = detect_root(view);
  size_t end = view.size();

  while (end > root.length && is_separator(path[end - 1]))
    --end;

  if (end == root.length && root.kind != RootKind::NONE)
    return root_result(path, root);

  size_t component_start = end;
  while (component_start > root.length &&
         !is_separator(path[component_start - 1])) {
    --component_start;
  }

  if (component_start == 0)
    return dot_literal();

  if (component_start == root.length)
    return root.kind == RootKind::NONE ? dot_literal() : root_result(path, root);

  size_t cut = component_start;
  while (cut > root.length && is_separator(path[cut - 1]))
    --cut;

  if (cut == 0)
    return dot_literal();

  if (cut == root.length)
    return root_result(path, root);

  return truncate_and_return(path, cut);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
