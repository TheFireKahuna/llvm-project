# =============================================================================
# Tests for memory/posix/ — P0 foundations.
# =============================================================================

# Context-specific NTSTATUS → errno overrides plus the ErrorOr<T> →
# negative-errno helper. The generic NTSTATUS table is exercised by
# `nt_pal_test`; this suite covers the three call sites where POSIX
# mandates an errno different from the generic mapping.
add_libc_unittest(
  posix_errno_test
  SUITE
    libc-osutil-tests
  SRCS
    posix_errno_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.error_or
    libc.src.__support.OSUtil.windows.memory_posix_errno
    libc.src.__support.OSUtil.windows.ntdll
)

# Page rounding, overflow-safe size checks, prot validation, flag
# validators for mmap / mremap / msync, madvise advice-code whitelist.
add_libc_unittest(
  posix_validation_test
  SUITE
    libc-osutil-tests
  SRCS
    posix_validation_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.OSUtil.windows.memory_posix_validation
    libc.src.__support.OSUtil.windows.ntdll
)

# AcquireMeta builders, one per POSIX call shape. Round-trip from POSIX
# inputs to fully populated AcquireMeta and confirm every region_flag
# bit matches the POSIX flag input.
add_libc_unittest(
  posix_meta_test
  SUITE
    libc-osutil-tests
  SRCS
    posix_meta_test.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.memory_posix_meta
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.va_region_desc
)

# Twelve DescMutator callbacks. Each test constructs a stack-resident
# RegionDesc, applies one mutator, and confirms (a) the target field
# changed, (b) unrelated flags bits stay intact under the OR / AND-NOT
# preservation pattern.
add_libc_unittest(
  posix_mutators_test
  SUITE
    libc-osutil-tests
  SRCS
    posix_mutators_test.cpp
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.memory_posix_mutators
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.va_region_desc
)
