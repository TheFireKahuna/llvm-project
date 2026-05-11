# =============================================================================
# Windows memory subsystem tests for the NTPOSIX va_tracker substrate.
# =============================================================================

add_subdirectory(posix)

# va_tracker outer ART (P1.G L3) — ROWEX-synchronised Adaptive Radix Tree
# correctness suite. Pins layout invariants, the load-bearing null-child
# filter for Node16 (Leis 2016 ROWEX phantom-match defence), grow paths
# (4→16→48→256), shrink-hysteresis edges (256→48 at 37, 48→16 at 12,
# 16→4 at 3), tagged-pointer leaf round-trip, addPrefixBefore prefix
# fusion, optimistic-prefix path, append-only insertion order under a
# concurrent reader, and ordered walk_range traversal across mixed
# node types.
add_libc_unittest(
  art_index_test
  SUITE
    libc-osutil-tests
  SRCS
    art_index_test.cpp
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.alloc_init_latch
    libc.src.__support.OSUtil.windows.art_index
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
)
