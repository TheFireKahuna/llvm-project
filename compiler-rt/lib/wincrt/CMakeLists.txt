# wincrt, the start-up library of Windows Itanium images, and the import
# libraries the driver links beside it. Built with the builtins, as the C
# runtime start files of other targets are, since the C++ runtimes link it.

add_compiler_rt_component(crt)

set(WINCRT_SOURCES
  atexit.cpp
  # One entry point per file, so that the linker extracts only the one the
  # program's main function selects.
  entry_dll.cpp
  entry_main.cpp
  entry_winmain.cpp
  entry_wmain.cpp
  entry_wwinmain.cpp
  winmain_main.cpp
  cfguard.cpp
  cfguard_dispatch.S
  delayload.cpp
  gs_cookie.S
  gshandler.cpp
  init.cpp
  loadconfig.cpp
  purecall.cpp
  security.cpp
  tls.cpp
  ucrt_math.c
  ucrt_stdio.c
  )

# The parts of wincrt that serve the whole process, the termination
# registries, exit, raise, abort, the thread start and rand_s, which every
# image imports from clang_rt.wincrt_dynamic.dll, and which
# clang_rt.wincrt_static.lib holds for a program linked with -static.
set(WINCRT_ATEXIT_SOURCES
  cxa_atexit.cpp
  cxa_thread_atexit.cpp
  exit.cpp
  rand_s.cpp
  signal.cpp
  thread.cpp
  )

# wincrt's own indirect calls are guarded, and its objects list the functions
# whose address they take, so that a guarded image grants only those.
set(WINCRT_CFLAGS -fno-exceptions -fno-rtti -mguard=cf)

find_program(WINCRT_LLVM_LIB NAMES llvm-lib
  HINTS ${LLVM_RUNTIME_OUTPUT_INTDIR} ${LLVM_TOOLS_BINARY_DIR})
if(NOT WINCRT_LLVM_LIB)
  message(FATAL_ERROR "llvm-lib is required to build the wincrt import libraries")
endif()

# Generates the import library <name>.lib from a module-definition file and
# installs it beside wincrt.
function(add_wincrt_import_library arch name def)
  get_compiler_rt_output_dir(${arch} output_dir)
  get_compiler_rt_install_dir(${arch} install_dir)
  set(output "${output_dir}/${name}.lib")
  if(arch STREQUAL "aarch64")
    set(machine arm64)
  else()
    set(machine x64)
  endif()
  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
    COMMAND ${WINCRT_LLVM_LIB} /nologo /machine:${machine}
            /def:${CMAKE_CURRENT_SOURCE_DIR}/${def} /out:${output}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${def}
    COMMENT "Generating ${name}.lib"
    VERBATIM)
  get_filename_component(target ${def} NAME_WE)
  add_custom_target(clang_rt.${target}-${arch} DEPENDS "${output}")
  add_dependencies(crt clang_rt.${target}-${arch})
  install(FILES "${output}" DESTINATION "${install_dir}" COMPONENT crt)
endfunction()

foreach(arch ${BUILTIN_SUPPORTED_ARCH})
  add_compiler_rt_runtime(clang_rt.wincrt
    STATIC
    ARCHS ${arch}
    SOURCES ${WINCRT_SOURCES}
    ADDITIONAL_HEADERS wincrt.h
    CFLAGS ${WINCRT_CFLAGS}
    CXX_STANDARD 17
    PARENT_TARGET crt)
  add_compiler_rt_runtime(clang_rt.wincrt_static
    STATIC
    ARCHS ${arch}
    SOURCES ${WINCRT_ATEXIT_SOURCES}
    ADDITIONAL_HEADERS wincrt.h
    CFLAGS ${WINCRT_CFLAGS}
    CXX_STANDARD 17
    PARENT_TARGET crt)
  # The DLL links like any image of the target, with wincrt and the import
  # libraries built here, but neither the C++ runtimes nor its own import
  # library.
  add_compiler_rt_runtime(clang_rt.wincrt
    SHARED
    ARCHS ${arch}
    SOURCES ${WINCRT_ATEXIT_SOURCES}
    ADDITIONAL_HEADERS wincrt.h
    CFLAGS ${WINCRT_CFLAGS}
    LINK_FLAGS -nostdlib++ --unwindlib=none
               -Wl,-nodefaultlib:clang_rt.wincrt_dynamic.lib
    DEPS clang_rt.builtins-${arch} clang_rt.wincrt-${arch}
         clang_rt.aligned_alloc-${arch} clang_rt.ucrt_memory-${arch}
         clang_rt.oldnames-${arch}
    CXX_STANDARD 17
    PARENT_TARGET crt)
  # Functions that ucrtbase.dll exports but the SDK's ucrt.lib does not;
  # Visual C++ imports them through vcruntime.lib.
  set_output_name(ucrt_memory_name clang_rt.ucrt_memory ${arch})
  add_wincrt_import_library(${arch} ${ucrt_memory_name} ucrt_memory.def)
  # The POSIX names of Universal CRT functions, under the name Visual C++
  # gives the library.
  add_wincrt_import_library(${arch} oldnames oldnames.def)

  # An empty math library: the Universal CRT provides the math functions, and
  # POSIX build systems link them with -lm.
  get_compiler_rt_output_dir(${arch} output_dir)
  get_compiler_rt_install_dir(${arch} install_dir)
  file(CONFIGURE OUTPUT ${output_dir}/m.lib CONTENT "!<arch>\n"
       NEWLINE_STYLE LF)
  install(FILES ${output_dir}/m.lib DESTINATION ${install_dir} COMPONENT crt)
endforeach()
