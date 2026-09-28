# wincrt, the start-up library of Windows Itanium images, and the import
# libraries the driver links beside it. Built with the builtins, as the C
# runtime start files of other targets are, since the C++ runtimes link it.

add_compiler_rt_component(crt)

set(WINCRT_SOURCES
  # One entry point per file, so that the linker extracts only the one the
  # program's main function selects.
  entry_dll.cpp
  entry_main.cpp
  entry_winmain.cpp
  entry_wmain.cpp
  entry_wwinmain.cpp
  init.cpp
  loadconfig.cpp
  security.cpp
  tls.cpp
  ucrt_stdio.c
  )

set(WINCRT_CFLAGS -fno-exceptions -fno-rtti)

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
