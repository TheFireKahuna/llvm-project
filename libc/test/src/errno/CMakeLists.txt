if(NOT LLVM_LIBC_FULL_BUILD OR LIBC_TARGET_OS_IS_GPU)
  return()
endif()

add_custom_target(libc_errno_unittests)

set(libc_errno_h_test_deps libc.include.errno.__generated_hdr__)
if(NOT (CMAKE_CXX_COMPILER_TARGET MATCHES "windows-ntposix"
        OR LLVM_DEFAULT_TARGET_TRIPLE MATCHES "windows-ntposix"))
  list(APPEND libc_errno_h_test_deps libc.src.errno.errno)
endif()

add_libc_unittest(
  errno_test
  SUITE
    libc_errno_unittests
  SRCS
    errno_test.cpp
  DEPENDS
    libc.src.errno.errno
    libc.test.UnitTest.ErrnoCheckingTest
)

add_libc_unittest(
  errno_h_test
  NO_ERRNO_SETTER_MATCHER
  SUITE
    libc_errno_unittests
  SRCS
    errno_h_test.cpp
  DEPENDS
    ${libc_errno_h_test_deps}
)
