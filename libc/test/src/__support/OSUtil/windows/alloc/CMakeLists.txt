# VaSubstrate comprehensive hermetic test — drives every public entry
# point (acquire/release/try_reclaim/arena_count) single-threaded, plus
# multi-thread acquire/release stress and a producer/consumer cross-
# thread release pattern. Hermetic model (LibcTest.hermetic, no c.dll).
add_libc_hermetic(
  va_substrate_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    va_substrate_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.va_substrate
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
)

# SlabPool→VaSubstrate migration deliverable: every slab body returned by
# SlabPool (i.e. every malloc routed through the slab classes) must
# resolve as RegionShape::LIBC_INTERNAL through `g_mapping_table`. Drives
# the production `malloc`/`free` surface, then snapshots the resulting VA
# directly. Failure here means a slab body escaped substrate ownership.
#
# Hermetic model (matches sibling va_substrate_test): NTPOSIX gates the
# unittest path and only hermetic tests build cleanly here. Hermetic mode
# pulls in libc.startup.windows.crt1 + hermetic_runtime, so g_mapping_table
# / posix_alloc are fully booted by the time main() runs (same contract
# va_substrate_test relies on for `MappingTableReadyByMainEntry`).
add_libc_hermetic(
  slab_body_internal_coverage_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    slab_body_internal_coverage_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.OSUtil.windows.mapping_table
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.stdlib.free
    libc.src.stdlib.malloc
)

# Phase 3 deliverable: SlabRegistry's L1 root and every populated L2 / L3
# directory page is substrate-served and stamped LIBC_INTERNAL. Walks the
# live radix from a real slab base and snapshots each level. Also covers
# `pick_class_for_directory`'s full Small / Medium / Large / XLarge / Huge
# ladder synthetically — CI only ever hits one VA-width branch at runtime.
add_libc_hermetic(
  slab_registry_internal_coverage_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    slab_registry_internal_coverage_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.OSUtil.windows.mapping_table
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.slab_pool
    libc.src.__support.OSUtil.windows.slab_registry
    libc.src.__support.OSUtil.windows.va_substrate
    libc.src.stdlib.free
    libc.src.stdlib.malloc
)
