//===-- Misuse-invariant COFF/PE section registry ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Type-safe, link-time, read-only dispatch tables built from records
// merged by lld-link via `#pragma section`. The primitive behind
// `.libcops$*`, `.libclzr$*`, `.libcveh$*`, `.libcfin$*`, and friends.
//
// Two layouts:
//
// (A) Flat — unordered record set. Three sections per registry:
//       .<name>$A   start bookend (one zero-init Record, skipped by walker)
//       .<name>$M   every caller's record (merge order is undefined)
//       .<name>$Z   end bookend (one zero-init Record, skipped by walker)
//
// (B) Phased — records ordered by a caller-chosen phase 0..9. Twelve
//     sections per registry:
//       .<name>$A               start bookend
//       .<name>$P0 .. .<name>$P9   phase buckets (records in lower-phase
//                                  buckets walk first; within a bucket
//                                  linker merge order is undefined)
//       .<name>$Z               end bookend
//
// lld-link merges all subsections alphabetically into a single output
// section `.<name>`. Walk [start+1, end) as `Record*` forward, or via
// reverse() for reverse-phase traversal (used by teardown sweeps).
//
// Why this shape (vs. the earlier `.libcops$B..Y` letter-per-kind scheme):
//
//   - No caller-chosen letter => silent letter collisions impossible.
//     Under the old scheme, two TUs using LIBC_REGISTER_FILE_OPS(R, ...)
//     with different kind names produced distinct symbols, so the
//     linker did NOT error, and the walker populated two unrelated
//     kind slots from the same letter. That attack is gone here --
//     every record lands in `$M` and uniqueness is enforced by the
//     caller-supplied `tag`, which becomes part of a strong symbol
//     name (duplicate-tag => duplicate-symbol linker error).
//
//   - Section name is owned by the DECLARE macro. Callers cannot
//     typo a section name or squat on another registry.
//
//   - `read`-only attribute is set in one place (the DECLARE macro).
//     A downstream TU that tries `#pragma section(".libcops$M", read,
//     write)` would union in MEM_WRITE -- caught at Tier B init by
//     verify_section_readonly().
//
//   - Bookends are typed. The walker iterates `Record*` directly; no
//     reinterpret_cast at each use-site.
//
// Usage — flat (three moving parts):
//
//   1) In the registry's owner header, declare once at file scope:
//
//        LIBC_DECLARE_SECTION_REGISTRY(libcops)
//
//   2) In exactly one TU, define the bookends and the range factory:
//
//        LIBC_DEFINE_SECTION_BOOKENDS(libcops, FileOpsRegistration)
//
//      Walk:
//
//        for (const auto &rec : libc_libcops_registry()) { ... }
//
//   3) In any TU that wants to contribute, at namespace scope:
//
//        LIBC_SECTION_REGISTER(libcops, FileOpsRegistration, Epoll,
//                              {FileKind::Epoll, &epoll_fd_ops})
//
// Usage — phased:
//
//   1) LIBC_DECLARE_SECTION_REGISTRY_PHASED(libcfin)
//   2) LIBC_DEFINE_SECTION_BOOKENDS(libcfin, FiniEntry)  // unchanged
//   3) LIBC_SECTION_REGISTER_PHASED(libcfin, FiniEntry, 7, alpc_bus,
//                                   {&alpc_bus_fini})
//
//   Reverse walk (teardown):
//
//     for (const auto &rec : libc_libcfin_registry().reverse()) { ... }
//
//   The `phase` argument must be a digit 0..9 that the preprocessor can
//   resolve. Both literal digits and `#define`-style symbolic constants
//   (e.g. `#define FINI_PHASE_SERVICES 7`) work — phase is expanded
//   before the dispatch paste, so symbolic phases route to the same
//   bucket as their literal value. Anything the preprocessor cannot
//   resolve to one of `0`..`9` (out-of-range literals, C++ `enum` /
//   `constexpr` constants, arbitrary expressions) fails at compile time
//   with "use of undeclared identifier `LIBC__SR_PHASED_BUCKET_<x>`".
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECTION_REGISTRY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECTION_REGISTRY_H

#include "include/__llvm-libc-common.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Typed view over a `.<name>$A..$Z` merged section range.
// begin()/end() skip the $A and $Z sentinels, so iteration yields only
// real records ($M for flat, $P0..$P9 for phased).
template <typename Record> class SectionRegistry {
public:
  // Range adapter returning records in reverse linker order. Iteration
  // order is `$P9 .. $P0` (phased) or the exact reverse of forward walk
  // (flat). Intended for teardown sweeps where tearing down in the
  // reverse order of registration is the desired behaviour.
  class ReverseView {
  public:
    class iterator {
    public:
      LIBC_INLINE constexpr explicit iterator(const Record *p) : p_(p) {}
      LIBC_INLINE const Record &operator*() const { return *(p_ - 1); }
      LIBC_INLINE iterator &operator++() {
        --p_;
        return *this;
      }
      LIBC_INLINE bool operator!=(const iterator &o) const {
        return p_ != o.p_;
      }

    private:
      const Record *p_;
    };

    LIBC_INLINE constexpr ReverseView(const Record *begin, const Record *end)
        : begin_(begin), end_(end) {}
    LIBC_INLINE iterator begin() const { return iterator(end_); }
    LIBC_INLINE iterator end() const { return iterator(begin_); }

  private:
    const Record *begin_;
    const Record *end_;
  };

  LIBC_INLINE constexpr SectionRegistry(const Record *start, const Record *end)
      : start_(start), end_(end) {}

  LIBC_INLINE const Record *begin() const { return start_ + 1; }
  LIBC_INLINE const Record *end() const { return end_; }
  LIBC_INLINE ReverseView reverse() const {
    return ReverseView(start_ + 1, end_);
  }

  // Address guaranteed to lie inside the merged output section at image
  // map time. Pass to verify_section_readonly().
  LIBC_INLINE const void *probe_address() const { return start_; }

private:
  const Record *start_;
  const Record *end_;
};

// Walks the PE section headers of the image at `image_base` (pass
// g_pcb.module_handle), finds the output section containing
// `in_section_addr`, and returns true iff that section's
// Characteristics have MEM_READ set and MEM_WRITE clear.
//
// False return means a downstream TU unioned MEM_WRITE into the section
// via `#pragma section(..., read, write)`. Callers should treat this as
// a fatal init-time invariant violation.
bool verify_section_readonly(const void *image_base,
                             const void *in_section_addr);

// Release-mode hard-fail wrapper around verify_section_readonly. Performs
// the same check unconditionally (NOT debug-only) and terminates the
// process via NtTerminateProcess if the section is writable. Use at every
// section-walker entry: the cost is one image-header walk, the alternative
// is silently shipping a writable dispatch table that an arbitrary-write
// primitive can repoint to attacker-controlled code.
//
// `section_name` is a string literal used purely for the abort code path
// (currently consumed by the bugcheck status only — the loader does not
// surface it — but kept as a future hook for a structured diagnostic).
[[noreturn]] void
fail_section_writable(const char *section_name);

LIBC_INLINE void enforce_section_readonly_or_fastfail(
    const void *image_base, const void *in_section_addr,
    const char *section_name) {
  if (!verify_section_readonly(image_base, in_section_addr))
    fail_section_writable(section_name);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Define the $A/$Z bookends and a factory returning a typed view of the
// merged range. Place in exactly one TU per registry. `record_type`
// must be aggregate-initializable with `{}`.
#define LIBC_DEFINE_SECTION_BOOKENDS(name, record_type)                        \
  namespace LIBC_NAMESPACE_DECL {                                              \
  namespace internal {                                                         \
  __LIBC_SECTION_ATTR("." #name "$A")                                          \
      static const record_type libc_##name##_section_start[1] = {{}};          \
  __LIBC_SECTION_ATTR("." #name "$Z")                                          \
      static const record_type libc_##name##_section_end[1] = {{}};            \
  static inline ::LIBC_NAMESPACE::internal::SectionRegistry<record_type>       \
  libc_##name##_registry() {                                                   \
    return ::LIBC_NAMESPACE::internal::SectionRegistry<record_type>{           \
        libc_##name##_section_start, libc_##name##_section_end};               \
  }                                                                            \
  }                                                                            \
  }

// Emit a single record into `.<name>$M`. `tag` must be unique across the
// entire link for this registry -- duplicates produce a linker
// duplicate-symbol error (record variable name embeds the tag).
//
// Trailing arguments are the brace-enclosed aggregate initializer for
// `record_type`. Variadic so that embedded commas (common in brace
// initializers) are not misparsed as macro argument separators.
//
// `[[gnu::retain]]` inhibits linker dead-strip: on COFF/Clang the
// attribute emits a `/INCLUDE:<symbol>` linker directive into `.drectve`,
// which lld-link honours through `/OPT:REF`. Without it, a TU whose only
// outward reference is its registry record (common for "fire-and-forget"
// filter registrations) could be dropped silently when nothing else in
// that TU is referenced.
//
// Must expand at namespace scope (file scope, outside any namespace {}
// block).
#define LIBC_SECTION_REGISTER(name, record_type, tag, ...)                     \
  namespace LIBC_NAMESPACE_DECL {                                              \
  namespace internal {                                                         \
  [[gnu::retain]] __LIBC_SECTION_ATTR("." #name "$M")                          \
      extern const record_type libc_##name##_entry_##tag = __VA_ARGS__;        \
  }                                                                            \
  }

// Emit a single record into `.<name>$P<phase>`. `phase` must resolve to
// a digit 0..9 after preprocessor expansion (literal digit OR a #define
// constant — the dispatch helpers below expand `phase` once before
// pasting it into a per-bucket macro name). Out-of-range literals and
// non-preprocessor constants paste into an undefined helper name and
// fail at compile time — preferred over the silent
// alphabetically-misordered section the naive `__declspec(allocate("."
// #name "$P" #phase))` form would produce.
//
// `tag` uniqueness rules match LIBC_SECTION_REGISTER. Must expand at
// namespace scope (file scope, outside any namespace {} block).
#define LIBC_SECTION_REGISTER_PHASED(name, record_type, phase, tag, ...)       \
  LIBC__SR_PHASED_DISPATCH_(name, record_type, phase, tag, __VA_ARGS__)

// Internal: receives `phase` already-expanded (parameter substitution in
// LIBC_SECTION_REGISTER_PHASED's body fully expands `phase` because it is
// not adjacent to # or ##). The `##` paste here therefore concatenates
// the expanded digit, not the caller's source token, producing
// `LIBC__SR_PHASED_BUCKET_<digit>`. An undefined helper name (caller
// passed something that did not expand to 0..9) fails the rescan.
#define LIBC__SR_PHASED_DISPATCH_(name, record_type, phase, tag, ...)          \
  LIBC__SR_PHASED_BUCKET_##phase(name, record_type, tag, __VA_ARGS__)

// Internal: per-bucket specialisations. The suffix is hard-coded into
// each, so __LIBC_SECTION_ATTR("." #name "$P0") etc. are guaranteed
// to match a section that LIBC_DECLARE_SECTION_REGISTRY_PHASED declared.
#define LIBC__SR_PHASED_AT_(name, record_type, suffix, tag, ...)               \
  namespace LIBC_NAMESPACE_DECL {                                              \
  namespace internal {                                                         \
  [[gnu::retain]] __LIBC_SECTION_ATTR("." #name "$" #suffix)                   \
      extern const record_type libc_##name##_entry_##tag = __VA_ARGS__;        \
  }                                                                            \
  }

#define LIBC__SR_PHASED_BUCKET_0(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P0, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_1(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P1, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_2(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P2, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_3(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P3, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_4(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P4, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_5(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P5, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_6(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P6, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_7(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P7, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_8(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P8, tag, __VA_ARGS__)
#define LIBC__SR_PHASED_BUCKET_9(name, record_type, tag, ...)                  \
  LIBC__SR_PHASED_AT_(name, record_type, P9, tag, __VA_ARGS__)

// Force-pull every archive member that contributes a symbol matching the
// glob pattern. Backed by lld-COFF /includeglob: in `.drectve`, deferred
// to after the linker's main convergence loop so the pattern sees the
// fully-populated lazy-archive symbol table.
//
// Use in the always-pulled dispatcher TU of a section registry to drag
// in every contributor whose only outward artifact is a `[[gnu::retain]]`
// section record. The matching anchor symbols are emitted by the per-
// registry registration macros (`LIBC_REGISTER_VEH_FILTER` etc.) using a
// fixed `__libc_<reg>_anchor_<tag>` convention.
//
// `pattern_str` must be a string literal so the preprocessor can splice
// it into the inline-asm directive at compile time. Must expand at
// namespace scope (no enclosing function or namespace block).
#define LIBC_FORCE_PULL_GLOB(pattern_str)                                      \
  __asm__(".pushsection .drectve, \"yn\"\n"                                    \
          ".ascii \" /includeglob:" pattern_str "\"\n"                         \
          ".popsection\n");                                                    \
  static_assert(true, "trailing-semicolon hook")

// Defines an externally-visible anchor function whose only purpose is to
// be matched by a `LIBC_FORCE_PULL_GLOB("__libc_anchor_<group>_*")`
// directive in some always-pulled dispatcher TU. Use only when there is
// no `LIBC_REGISTER_*` call in the TU (those macros emit their own
// per-registry anchor); the prime example is a strong override of a
// weak symbol whose declaring TU would otherwise drop it.
#define LIBC_ANCHOR_DEFINE(group, tag)                                         \
  extern "C" [[gnu::used]] void __libc_anchor_##group##_##tag(void) {}         \
  static_assert(true, "trailing-semicolon hook")

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECTION_REGISTRY_H
