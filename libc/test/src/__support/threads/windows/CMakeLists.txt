add_custom_target(libc-support-threads-windows-tests)

add_libc_unittest(
  mutex_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    mutex_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.mutex
    libc.src.__support.threads.raw_mutex
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.mutex
    libc.src.__support.threads.windows.thread
    libc.src.__support.time.clock_gettime
    libc.test.UnitTest.Test
)

add_libc_unittest(
  thread_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    thread_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.mutex
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.mutex
    libc.src.__support.threads.windows.thread
    libc.test.UnitTest.Test
)

add_libc_unittest(
  cndvar_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    cndvar_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.mutex
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.CndVar
    libc.src.__support.threads.windows.mutex
    libc.src.__support.threads.windows.thread
    libc.src.__support.time.clock_gettime
    libc.test.UnitTest.Test
)

add_libc_unittest(
  barrier_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    barrier_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.pthread_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.barrier
    libc.src.__support.threads.windows.thread
    libc.test.UnitTest.Test
)

add_libc_unittest(
  futex_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    futex_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.sleep
    libc.src.__support.threads.windows.futex_utils
    libc.test.UnitTest.Test
)

add_libc_unittest(
  futex_stress_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    futex_stress_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.windows.futex_utils
    libc.test.UnitTest.Test
)

_get_common_test_compile_options(futex_bench_compile_options "" "")
get_link_options(futex_bench_link_options ${futex_bench_compile_options})

get_fq_deps_list(futex_bench_deps
  libc.hdr.errno_macros
  libc.src.__support.CPP.atomic
  libc.src.__support.threads.windows.futex_utils
  libc.src.__support.OSUtil.windows.vt_pty
  libc.src.__support.OSUtil.windows.console_tty
  libc.src.__support.OSUtil.windows.terminal_ops
  libc.src.__support.OSUtil.windows.console
  libc.src.__support.OSUtil.windows.epoll_ops
  libc.src.__support.File.file
  libc.src.__support.File.windows.file
  libc.src.__support.File.windows.stdin
  libc.src.__support.File.windows.stdout
  libc.src.__support.File.windows.stderr
  libc.src.__support.threads.thread
  libc.src.__support.threads.fork_callbacks
  libc.src.compiler.generic.__stack_chk_fail
)
get_object_files_for_test(
  futex_bench_objs futex_bench_skipped futex_bench_link_libraries
  ${futex_bench_deps})

add_executable(
  futex_bench
  EXCLUDE_FROM_ALL
  futex_bench.cpp
)
target_include_directories(futex_bench SYSTEM PRIVATE ${LIBC_INCLUDE_DIR})
target_include_directories(futex_bench PRIVATE ${LIBC_SOURCE_DIR})
target_include_directories(futex_bench PRIVATE ${LIBC_BUILD_DIR})
target_compile_options(futex_bench PRIVATE ${futex_bench_compile_options})
target_link_directories(futex_bench PRIVATE ${LLVM_BINARY_DIR}/lib)
target_link_options(futex_bench PRIVATE ${futex_bench_link_options}
                    -Xlinker /FORCE:MULTIPLE)
set_target_properties(
  futex_bench
  PROPERTIES
    CXX_STANDARD ${CMAKE_CXX_STANDARD}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(futex_bench PRIVATE ${futex_bench_objs}
                                         ${futex_bench_link_libraries}
                                         windows_ntdll
                                         windows_sspicli)
add_dependencies(futex_bench ${futex_bench_deps})
add_dependencies(libc-support-threads-windows-tests futex_bench)
if(TARGET libc-unit-tests-build)
  add_dependencies(libc-unit-tests-build futex_bench)
endif()
