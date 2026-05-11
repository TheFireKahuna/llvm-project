add_custom_target(libc_libgen_windows_unittests)

add_libc_unittest(
  libgen_test
  SUITE
    libc_libgen_windows_unittests
  SRCS
    libgen_test.cpp
  DEPENDS
    libc.src.libgen.basename
    libc.src.libgen.dirname
)
