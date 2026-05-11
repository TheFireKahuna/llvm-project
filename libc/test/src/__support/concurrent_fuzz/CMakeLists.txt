# concurrent_fuzz — property-based linearizability fuzzer framework.
#
# Generic over SUT/Oracle pair via type-erased function pointers; the
# only OS-specific code lives in `worker_pool.cpp` (NT thread spawn +
# `ThreadSelectedCpuSets` affinity). Future Linux ports add a sibling
# `linux/worker_pool.cpp` and a CMake branch.
#
# Builds on Windows-NTPOSIX only today — guarded by the parent
# `add_subdirectory` predicate.

if(NOT LIBC_TARGET_OS_IS_WINDOWS)
  return()
endif()

add_object_library(
  concurrent_fuzz
  SRCS
    siphash.cpp
    schedule.cpp
    history.cpp
    linearizability.cpp
    shrinker.cpp
    worker_pool.cpp
  HDRS
    op.h
    oracle.h
    random.h
    siphash.h
    schedule.h
    history.h
    linearizability.h
    shrinker.h
    worker_pool.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
)
