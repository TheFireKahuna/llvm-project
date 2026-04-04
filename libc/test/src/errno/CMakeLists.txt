if(NOT LLVM_LIBC_FULL_BUILD OR LIBC_TARGET_OS_IS_GPU)
  return()
endif()

add_custom_target(libc_errno_unittests)

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

# errno_h_test exercises the public <errno.h> contract: macro expansion of
# `errno`, presence/value of POSIX error codes, and a direct call to
# __llvm_libc_errno(). NO_ERRNO_SETTER_MATCHER skips the implicit
# error_to_string dep, which would otherwise pull the full errno entrypoint
# chain into this header-only contract test.
add_libc_unittest(
  errno_h_test
  NO_ERRNO_SETTER_MATCHER
  SUITE
    libc_errno_unittests
  SRCS
    errno_h_test.cpp
  DEPENDS
    libc.include.errno.__generated_hdr__
    libc.src.errno.errno
)
