# aligned_alloc and posix_memalign, which the Universal CRT lacks, for Windows
# Itanium images. Built beside wincrt, with the builtins' flags, since every
# image links it, the C++ runtimes' included.

foreach(arch ${BUILTIN_SUPPORTED_ARCH})
  add_compiler_rt_runtime(clang_rt.aligned_alloc
    STATIC
    ARCHS ${arch}
    SOURCES aligned_alloc.c
    CFLAGS ${BUILTIN_CFLAGS}
    PARENT_TARGET crt)
endforeach()
