//===-- Implementation of scandir -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "scandir.h"

#include "src/__support/File/dir.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/stdlib/free.h"
#include "src/stdlib/malloc.h"
#include "src/stdlib/qsort_r.h"
#include "src/stdlib/realloc.h"

#include <dirent.h>
#include <limits.h>

namespace LIBC_NAMESPACE_DECL {

namespace {

void free_list(struct ::dirent **list, size_t count) {
  for (size_t i = 0; i < count; ++i)
    LIBC_NAMESPACE::free(list[i]);
  LIBC_NAMESPACE::free(list);
}

// qsort_r trampoline. The user's comparator is passed as the context arg.
// qsort_r passes pointers to array elements — each element is a
// struct dirent*, so a and b are effectively struct dirent**.
int compar_trampoline(const void *a, const void *b, void *arg) {
  auto compar = reinterpret_cast<int (*)(const struct ::dirent **,
                                         const struct ::dirent **)>(arg);
  const auto *lhs = static_cast<struct ::dirent *const *>(a);
  const auto *rhs = static_cast<struct ::dirent *const *>(b);
  const struct ::dirent *lhs_entry = *lhs;
  const struct ::dirent *rhs_entry = *rhs;
  return compar(&lhs_entry, &rhs_entry);
}

} // namespace

LLVM_LIBC_FUNCTION(int, scandir,
                   (const char *dirp, struct ::dirent ***namelist,
                    int (*filter)(const struct ::dirent *),
                    int (*compar)(const struct ::dirent **,
                                 const struct ::dirent **))) {
  auto dir = Dir::open(dirp);
  if (!dir) {
    libc_errno = dir.error();
    return -1;
  }

  Dir *d = dir.value();
  struct ::dirent **list = nullptr;
  size_t count = 0;
  size_t capacity = 0;
  int err = 0;

  for (;;) {
    auto result = d->read();
    if (!result) {
      err = result.error();
      break;
    }

    struct ::dirent *entry = result.value();
    if (entry == nullptr)
      break;

    if (filter && !filter(entry))
      continue;

    // Guard before allocating — scandir returns int.
    if (count >= static_cast<size_t>(INT_MAX)) {
      err = ENOMEM;
      break;
    }

    // Grow the pointer array when full.
    if (count >= capacity) {
      size_t new_cap = capacity ? capacity * 2 : 16;
      auto *new_list = static_cast<struct ::dirent **>(LIBC_NAMESPACE::realloc(
          list, new_cap * sizeof(struct ::dirent *)));
      if (!new_list) {
        err = ENOMEM;
        break;
      }
      list = new_list;
      capacity = new_cap;
    }

    // Deep-copy — readdir's buffer is reused on next call.
    size_t entry_size = entry->d_reclen;
    auto *copy =
        static_cast<struct ::dirent *>(LIBC_NAMESPACE::malloc(entry_size));
    if (!copy) {
      err = ENOMEM;
      break;
    }
    __builtin_memcpy(copy, entry, entry_size);
    list[count++] = copy;
  }

  d->close();

  if (err) {
    free_list(list, count);
    libc_errno = err;
    return -1;
  }

  if (compar && count > 1) {
    LIBC_NAMESPACE::qsort_r(list, count, sizeof(struct ::dirent *),
                             compar_trampoline,
                             reinterpret_cast<void *>(compar));
  }

  *namelist = list;
  return static_cast<int>(count);
}

} // namespace LIBC_NAMESPACE_DECL
