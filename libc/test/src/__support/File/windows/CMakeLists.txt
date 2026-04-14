add_libc_test(
  file_test
  SUITE
    libc-support-tests
  SRCS
    file_test.cpp
  DEPENDS
    libc.hdr.stdio_macros
    libc.hdr.types.off_t
    libc.src.__support.File.file
    libc.src.__support.File.windows.file
)

add_subdirectory(testdata)
