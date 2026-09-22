//===-- aligned_alloc.cpp - C11 aligned_alloc over UCRT malloc bases ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// C17 7.22.3.1 aligned_alloc, which the UCRT neither declares nor exports and
// which Microsoft documents as unlikely to ever exist. This member supplies it
// for the powers-of-two alignments up to 16.
//
// malloc bases on Win64 are 16-aligned by contract: HeapAlloc aligns to
// MEMORY_ALLOCATION_ALIGNMENT (winnt.h), 16 on _WIN64, and the guarantee holds
// for the legacy NT heap and the segment heap alike. A malloc base is
// therefore a valid aligned_alloc result for every alignment up to 16, and
// free() releases it: the standard's contract holds with no metadata, no
// free() interception and no state, inside an image and across images.
//
// Above 16 the UCRT offers no suitably aligned base-pointer allocation.
// _aligned_malloc returns an interior pointer even for alignments <= 16;
// its malloc base lives in a private slot (UCRT
// heap/align.cpp), so free() cannot release it -- Microsoft's documentation
// makes free() on an _aligned_malloc result illegal. Routing aligned_alloc of
// a larger alignment through it would turn a standard call into that trap, and
// teaching free() to classify interior pointers has no sound implementation
// here: a slot-read heuristic can misfire on an ordinary block, and a registry
// of live blocks cannot be process-wide from a per-image static runtime (see
// docs/WindowsItanium/OperatorDelete.md for the cross-image allocator
// contract). These extended alignments are unsupported by aligned_alloc,
// which must return null for them under C17 7.22.3.1. Callers that need one use
// _aligned_malloc/_aligned_free directly, or aligned operator new, both of
// which pair within themselves.
//
// Parameter validation happens here, before any UCRT call: the _aligned_*
// family routes invalid parameters through the invalid-parameter handler,
// while C17 requires a null pointer for unsupported alignments. Setting
// EINVAL for those requests and ENOMEM for excessive sizes is our policy,
// not an ISO C requirement. Zero-size requests return null.
//
// WG14 DR 460's final correction (adopted in C17) removed C11's requirement
// that size be a multiple of alignment. Do not reject non-multiple sizes.
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <malloc.h>
#include <stddef.h>

// MEMORY_ALLOCATION_ALIGNMENT from winnt.h, spelled here to keep the member's
// includes off the Windows headers.
#define WINCRT_MAX_ALIGNED_ALLOC_ALIGNMENT 16

extern "C" void *__cdecl aligned_alloc(size_t alignment, size_t size) {
  // Only powers of two up to the platform's malloc alignment are supported.
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      alignment > WINCRT_MAX_ALIGNED_ALLOC_ALIGNMENT) {
    errno = EINVAL;
    return nullptr;
  }

  if (size == 0)
    return nullptr;

  // The ceiling the UCRT allocation family enforces for any request.
  if (size > _HEAP_MAXREQ) {
    errno = ENOMEM;
    return nullptr;
  }

  return malloc(size);
}
