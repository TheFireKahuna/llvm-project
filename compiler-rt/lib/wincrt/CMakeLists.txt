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
# registries, exit, raise, abort, the thread start, rand_s and the Universal
# CRT's formatted I/O, which every image imports from
# clang_rt.wincrt_dynamic.dll, and which clang_rt.wincrt_static.lib holds for a
# program linked with -static.
set(WINCRT_ATEXIT_SOURCES
  cxa_atexit.cpp
  cxa_thread_atexit.cpp
  exit.cpp
  formatted_io.c
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
# installs it beside wincrt. Further module-definition files, given by full
# path, each name another DLL, and their import libraries are merged into it.
function(add_wincrt_import_library arch name def)
  get_compiler_rt_output_dir(${arch} output_dir)
  get_compiler_rt_install_dir(${arch} install_dir)
  set(output "${output_dir}/${name}.lib")
  if(arch STREQUAL "aarch64")
    set(machine arm64)
  else()
    set(machine x64)
  endif()
  set(parts)
  set(part_commands)
  foreach(part_def ${ARGN})
    get_filename_component(part_name ${part_def} NAME_WE)
    set(part "${CMAKE_CURRENT_BINARY_DIR}/${arch}/${part_name}.lib")
    list(APPEND parts "${part}")
    list(APPEND part_commands
      COMMAND ${WINCRT_LLVM_LIB} /nologo /machine:${machine}
              /def:${part_def} /out:${part})
  endforeach()
  if(parts)
    set(main "${CMAKE_CURRENT_BINARY_DIR}/${arch}/${name}.lib")
    list(APPEND part_commands
      COMMAND ${WINCRT_LLVM_LIB} /nologo /machine:${machine}
              /out:${output} ${main} ${parts})
  else()
    set(main "${output}")
  endif()
  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
            "${CMAKE_CURRENT_BINARY_DIR}/${arch}"
    COMMAND ${WINCRT_LLVM_LIB} /nologo /machine:${machine}
            /def:${CMAKE_CURRENT_SOURCE_DIR}/${def} /out:${main}
    ${part_commands}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${def} ${ARGN}
    COMMENT "Generating ${name}.lib"
    VERBATIM)
  get_filename_component(target ${def} NAME_WE)
  add_custom_target(clang_rt.${target}-${arch} DEPENDS "${output}")
  add_dependencies(crt clang_rt.${target}-${arch})
  install(FILES "${output}" DESTINATION "${install_dir}" COMPONENT crt)
endfunction()

# The Universal CRT functions that another import library a program may name
# offers too, which every entry object wraps. clang_rt.ucrt_memory.lib imports
# each __wrap_ name as the function, from the DLL that ucrt.lib, or
# ucrt_memory.def, imports the function itself from.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/shared_imports.h)
set(shared_import_entry "X\\(([A-Za-z0-9_]+), ([a-z]+)(, ([A-Za-z0-9_]+))?\\)")
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shared_imports.h shared_imports)
string(REGEX MATCHALL "${shared_import_entry}" shared_imports
       "${shared_imports}")
set(shared_import_modules)
foreach(entry ${shared_imports})
  string(REGEX MATCH "${shared_import_entry}" entry "${entry}")
  set(module ${CMAKE_MATCH_2})
  set(export ${CMAKE_MATCH_1})
  if(CMAKE_MATCH_4)
    set(export ${CMAKE_MATCH_4})
  endif()
  list(APPEND shared_import_modules ${module})
  string(APPEND shared_imports_${module}
    "    __wrap_${CMAKE_MATCH_1} EXPORTAS ${export}\n")
endforeach()
list(REMOVE_DUPLICATES shared_import_modules)
set(shared_import_defs)
foreach(module ${shared_import_modules})
  if(module STREQUAL "ucrtbase")
    set(dll ucrtbase.dll)
  else()
    set(dll api-ms-win-crt-${module}-l1-1-0.dll)
  endif()
  set(def "${CMAKE_CURRENT_BINARY_DIR}/shared_imports_${module}.def")
  file(CONFIGURE OUTPUT "${def}"
       CONTENT "LIBRARY ${dll}\nEXPORTS\n${shared_imports_${module}}"
       @ONLY NEWLINE_STYLE LF)
  list(APPEND shared_import_defs "${def}")
endforeach()

foreach(arch ${BUILTIN_SUPPORTED_ARCH})
  add_compiler_rt_runtime(clang_rt.wincrt
    STATIC
    ARCHS ${arch}
    SOURCES ${WINCRT_SOURCES}
    ADDITIONAL_HEADERS formatted_io.h shared_imports.h wincrt.h
    CFLAGS ${WINCRT_CFLAGS}
    CXX_STANDARD 17
    PARENT_TARGET crt)
  add_compiler_rt_runtime(clang_rt.wincrt_static
    STATIC
    ARCHS ${arch}
    SOURCES ${WINCRT_ATEXIT_SOURCES}
    ADDITIONAL_HEADERS formatted_io.h shared_imports.h wincrt.h
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
    ADDITIONAL_HEADERS formatted_io.h shared_imports.h wincrt.h
    CFLAGS ${WINCRT_CFLAGS}
    LINK_FLAGS -nostdlib++ --unwindlib=none
               -Wl,-nodefaultlib:clang_rt.wincrt_dynamic.lib
    DEPS clang_rt.builtins-${arch} clang_rt.wincrt-${arch}
         clang_rt.aligned_alloc-${arch} clang_rt.ucrt_memory-${arch}
         clang_rt.oldnames-${arch}
    CXX_STANDARD 17
    PARENT_TARGET crt)
  # Functions that ucrtbase.dll exports but the SDK's ucrt.lib does not;
  # Visual C++ imports them through vcruntime.lib. With them, the kernel32.dll
  # functions wincrt imports under names of its own.
  set_output_name(ucrt_memory_name clang_rt.ucrt_memory ${arch})
  add_wincrt_import_library(${arch} ${ucrt_memory_name} ucrt_memory.def
    ${shared_import_defs} ${CMAKE_CURRENT_SOURCE_DIR}/app_policy.def)
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

  # The manifest the driver embeds in every executable, beside wincrt, whose
  # start-up requires the segment heap it selects.
  configure_file(segment_heap.manifest
    ${output_dir}/segment_heap.manifest COPYONLY)
  install(FILES ${output_dir}/segment_heap.manifest
    DESTINATION ${install_dir} COMPONENT crt)
endforeach()
