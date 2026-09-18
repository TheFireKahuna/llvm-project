# Helpers for registering Windows Itanium wincrt ExternalProjects.
# Target selection and bootstrap dependency ordering live in llvm/runtimes.

# Windows Itanium CRT startup library. Built after builtins, before runtimes.
# Provides CRT entry points, __cxa_atexit, security cookie, and UCRT bridge.
function(wincrt_default_target compiler_rt_path)
  cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})

  set_enable_per_target_runtime_dir()

  llvm_ExternalProject_Add(wincrt
                           ${compiler_rt_path}/lib/wincrt
                           DEPENDS ${ARG_DEPENDS}
                           CMAKE_ARGS -DLLVM_LIBRARY_OUTPUT_INTDIR=${LLVM_LIBRARY_DIR}
                                      -DLLVM_RUNTIME_OUTPUT_INTDIR=${LLVM_TOOLS_BINARY_DIR}
                                      -DLLVM_DEFAULT_TARGET_TRIPLE=${LLVM_TARGET_TRIPLE}
                                      -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=${LLVM_ENABLE_PER_TARGET_RUNTIME_DIR}
                                      -DLLVM_CMAKE_DIR=${CMAKE_BINARY_DIR}
                                      -DCMAKE_C_COMPILER_WORKS=ON
                                      -DCMAKE_CXX_COMPILER_WORKS=ON
                                      -DCMAKE_ASM_COMPILER_WORKS=ON
                                      ${COMMON_CMAKE_ARGS}
                                      ${WINCRT_CMAKE_ARGS}
                           PASSTHROUGH_PREFIXES COMPILER_RT
                                                WINCRT
                           USE_TOOLCHAIN
                           TARGET_TRIPLE ${LLVM_TARGET_TRIPLE}
                           FOLDER "Compiler-RT"
                           ${EXTRA_ARGS})
endfunction()

function(wincrt_register_target compiler_rt_path name)
  cmake_parse_arguments(ARG "" "" "DEPENDS;CMAKE_ARGS;EXTRA_ARGS" ${ARGN})

  set(${name}_extra_args ${ARG_CMAKE_ARGS})
  get_cmake_property(variable_names VARIABLES)
  foreach(variable_name ${variable_names})
    string(FIND "${variable_name}" "WINCRT_${name}" out)
    if("${out}" EQUAL 0)
      string(REPLACE "WINCRT_${name}_" "" new_name ${variable_name})
      if(new_name STREQUAL CACHE_FILES)
        foreach(cache IN LISTS ${variable_name})
          list(APPEND ${name}_extra_args -C ${cache})
        endforeach()
      else()
        string(REPLACE ";" "|" new_value "${${variable_name}}")
        list(APPEND ${name}_extra_args "-D${new_name}=${new_value}")
      endif()
    endif()
  endforeach()

  llvm_ExternalProject_Add(wincrt-${name}
                           ${compiler_rt_path}/lib/wincrt
                           DEPENDS ${ARG_DEPENDS}
                           CMAKE_ARGS -DLLVM_LIBRARY_OUTPUT_INTDIR=${LLVM_LIBRARY_DIR}
                                      -DLLVM_RUNTIME_OUTPUT_INTDIR=${LLVM_TOOLS_BINARY_DIR}
                                      -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON
                                      -DLLVM_CMAKE_DIR=${CMAKE_BINARY_DIR}
                                      -DCMAKE_C_COMPILER_WORKS=ON
                                      -DCMAKE_CXX_COMPILER_WORKS=ON
                                      -DCMAKE_ASM_COMPILER_WORKS=ON
                                      -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON
                                      ${COMMON_CMAKE_ARGS}
                                      ${${name}_extra_args}
                           USE_TOOLCHAIN
                           FOLDER "Compiler-RT"
                           ${EXTRA_ARGS} ${ARG_EXTRA_ARGS})
endfunction()
