# wincrt, the start-up library of Windows Itanium images, and the import
# libraries the driver links beside it. Built with the builtins, as the C
# runtime start files of other targets are, since the C++ runtimes link it.

add_compiler_rt_component(crt)

set(WINCRT_SOURCES
  aligned_alloc.cpp
  atexit.cpp
  cfguard.cpp
  delayload.cpp
  # One entry point per file, so that the linker extracts only the one the
  # program's main function selects.
  entry_dll.cpp
  entry_main.cpp
  entry_winmain.cpp
  entry_wmain.cpp
  entry_wwinmain.cpp
  winmain_main.cpp
  init.cpp
  purecall.cpp
  tls.cpp
  tls_tail.cpp
  tlregdtor.cpp
  ucrt.c
  )

# The parts of wincrt that serve the whole process, the termination
# registries, exit, exec, raise, abort, assert, the thread start, rand_s and
# the Universal CRT's formatted I/O, which every image imports from
# clang_rt.wincrt_dynamic.dll, and which clang_rt.wincrt_static.lib holds for a
# program linked with -static.
set(WINCRT_PROCESS_SOURCES
  assert.cpp
  cxa_atexit.cpp
  cxa_thread_atexit.cpp
  exec.cpp
  exit.cpp
  formatted_io.c
  rand_s.cpp
  thread.cpp
  tlsplace.cpp
  tlsrt.cpp
  tp.S
  )

# vcruntime's support for an MSVC object's thread-locals and local statics,
# whose own thread-locals are the loader's: built for the MSVC environment, so
# that, like the objects that name them, they state no thread-local model.
set(WINCRT_MSVC_SOURCES
  thread_safe_statics.c
  tlsdyn.c
  )

# The C++ runtimes are built after wincrt, which uses none of their headers.
set(WINCRT_CFLAGS -fno-exceptions -fno-rtti -nostdinc++)
include_directories(../../include)

find_program(WINCRT_LLVM_LIB NAMES llvm-lib HINTS ${LLVM_TOOLS_BINARY_DIR}
             REQUIRED)

# Generates the import library <name>.lib from the module-definition file def,
# which is preprocessed so that it can take its names from ucrt_names.h, and
# installs it beside wincrt.
function(add_wincrt_import_library arch name def)
  get_compiler_rt_output_dir(${arch} output_dir)
  get_compiler_rt_install_dir(${arch} install_dir)
  set(output ${output_dir}/${name}.lib)
  set(expanded ${CMAKE_CURRENT_BINARY_DIR}/${name}.def)
  if(arch STREQUAL "aarch64")
    set(machine arm64)
  else()
    set(machine x64)
  endif()
  add_custom_command(OUTPUT ${output}
    COMMAND ${CMAKE_C_COMPILER} -E -P -x assembler-with-cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/${def} -o ${expanded}
    COMMAND ${WINCRT_LLVM_LIB} /nologo /machine:${machine} /def:${expanded}
            /out:${output}
    DEPENDS ${def} ucrt_names.h
    COMMENT "Generating ${name}.lib"
    VERBATIM)
  add_custom_target(${name}-${arch} DEPENDS ${output})
  add_dependencies(crt ${name}-${arch})
  install(FILES ${output} DESTINATION ${install_dir} COMPONENT crt)
endfunction()

foreach(arch ${BUILTIN_SUPPORTED_ARCH})
  # At the target's own hardware baseline, which the MSVC environment's
  # default is below.
  if(arch STREQUAL "aarch64")
    set(baseline -march=armv8.2-a)
  else()
    set(baseline -march=x86-64-v3)
  endif()
  add_compiler_rt_object_libraries(WincrtMSVC
    ARCHS ${arch}
    SOURCES ${WINCRT_MSVC_SOURCES}
    CFLAGS --target=${arch}-pc-windows-msvc ${baseline} -Xclang -cfguard)
  add_compiler_rt_runtime(clang_rt.wincrt
    STATIC
    ARCHS ${arch}
    SOURCES ${WINCRT_SOURCES}
    OBJECT_LIBS WincrtMSVC
    ADDITIONAL_HEADERS entry.h ucrt_names.h wincrt.h
    CFLAGS ${WINCRT_CFLAGS}
    CXX_STANDARD 17
    PARENT_TARGET crt)
  # Functions that ucrtbase.dll exports but the SDK's ucrt.lib does not.
  set_output_name(ucrt_memory_name clang_rt.ucrt_memory ${arch})
  add_wincrt_import_library(${arch} ${ucrt_memory_name} ucrt_memory.def)
  # The POSIX names of the Universal CRT's functions, under the name Visual
  # C++ gives the library.
  add_wincrt_import_library(${arch} oldnames oldnames.def)
  add_compiler_rt_runtime(clang_rt.wincrt_static
    STATIC
    ARCHS ${arch}
    SOURCES ${WINCRT_PROCESS_SOURCES}
    ADDITIONAL_HEADERS tlsrt.h ucrt_names.h wincrt.h
    CFLAGS ${WINCRT_CFLAGS}
    CXX_STANDARD 17
    PARENT_TARGET crt)
  # The DLL links like any image of the target, with wincrt, but neither the
  # C++ runtimes nor its own import library.
  add_compiler_rt_runtime(clang_rt.wincrt
    SHARED
    ARCHS ${arch}
    SOURCES ${WINCRT_PROCESS_SOURCES}
    ADDITIONAL_HEADERS tlsrt.h ucrt_names.h wincrt.h
    CFLAGS ${WINCRT_CFLAGS}
    LINK_FLAGS -nostdlib++ --unwindlib=none
               -Wl,-nodefaultlib:clang_rt.wincrt_dynamic.lib
    DEPS clang_rt.builtins-${arch} clang_rt.wincrt-${arch}
         ${ucrt_memory_name}-${arch} oldnames-${arch}
    CXX_STANDARD 17
    PARENT_TARGET crt)

  # An empty math library: the Universal CRT has the math functions, and POSIX
  # build systems link them with -lm.
  get_compiler_rt_output_dir(${arch} output_dir)
  get_compiler_rt_install_dir(${arch} install_dir)
  file(CONFIGURE OUTPUT ${output_dir}/m.lib CONTENT "!<arch>\n"
       NEWLINE_STYLE LF)
  # The manifest the driver embeds in every executable, whose start-up
  # requires the segment heap it selects.
  configure_file(segment_heap.manifest ${output_dir}/segment_heap.manifest
                 COPYONLY)
  install(FILES ${output_dir}/m.lib ${output_dir}/segment_heap.manifest
          DESTINATION ${install_dir} COMPONENT crt)
endforeach()

add_dependencies(compiler-rt crt)
