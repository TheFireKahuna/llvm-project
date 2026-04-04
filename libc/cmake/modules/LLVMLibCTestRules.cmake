function(_get_common_test_compile_options output_var c_test flags)
  _get_compile_options_from_flags(compile_flags ${flags})
  _get_compile_options_from_config(config_flags)
  _get_compile_options_from_arch(arch_flags)

  # Death test executor is only available in Linux for now.
  if(NOT ${LIBC_TARGET_OS} STREQUAL "linux")
    if (MSVC)
      list(REMOVE_ITEM config_flags "/DLIBC_ADD_NULL_CHECKS")
    else()
      list(REMOVE_ITEM config_flags "-DLIBC_ADD_NULL_CHECKS")
    endif()
  endif()

  set(compile_options
      ${LIBC_COMPILE_OPTIONS_DEFAULT}
      ${LIBC_TEST_COMPILE_OPTIONS_DEFAULT}
      ${compile_flags}
      ${config_flags}
      ${arch_flags})

  if(LLVM_LIBC_COMPILER_IS_GCC_COMPATIBLE)
    # -fpie is not supported on Windows.
    if(NOT LIBC_TARGET_OS_IS_WINDOWS)
      list(APPEND compile_options "-fpie")
    endif()

    if(LLVM_LIBC_FULL_BUILD)
      list(APPEND compile_options "-DLIBC_FULL_BUILD")
      # Only add -ffreestanding flag in full build mode.
      list(APPEND compile_options "-ffreestanding")
      list(APPEND compile_options "-fno-exceptions")
      if(LIBC_TARGET_OS_IS_WINDOWS)
        list(APPEND compile_options "-funwind-tables")
      else()
        list(APPEND compile_options "-fno-unwind-tables")
        list(APPEND compile_options "-fno-asynchronous-unwind-tables")
      endif()
      if(NOT c_test)
        list(APPEND compile_options "-fno-rtti")
      endif()
    endif()

    if(LIBC_COMPILER_HAS_FIXED_POINT)
      list(APPEND compile_options "-ffixed-point")
    endif()

    list(APPEND compile_options "-Wall")
    list(APPEND compile_options "-Wextra")
    # -DLIBC_WNO_ERROR=ON if you can't build cleanly with -Werror.
    if(NOT LIBC_WNO_ERROR)
      # list(APPEND compile_options "-Werror")
    endif()
    list(APPEND compile_options "-Wconversion")
    # FIXME: convert to -Wsign-conversion
    list(APPEND compile_options "-Wno-sign-conversion")
    list(APPEND compile_options "-Wimplicit-fallthrough")
    list(APPEND compile_options "-Wwrite-strings")
    # Silence this warning because _Complex is a part of C99.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      if(NOT c_test)
        list(APPEND compile_options "-fext-numeric-literals")
      endif()
    else()
      list(APPEND compile_options "-Wno-c99-extensions")
      list(APPEND compile_options "-Wno-gnu-imaginary-constant")
    endif()
    list(APPEND compile_options "-Wno-pedantic")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      list(APPEND compile_options "-Wstrict-prototypes")
      list(APPEND compile_options "-Wextra-semi")
      list(APPEND compile_options "-Wnewline-eof")
      list(APPEND compile_options "-Wnonportable-system-include-path")
      list(APPEND compile_options "-Wthread-safety")
    endif()
  endif()
  set(${output_var} ${compile_options} PARENT_SCOPE)
endfunction()

function(_get_hermetic_test_compile_options output_var)
  _get_common_test_compile_options(compile_options "" "")
  libc_add_definition(compile_options "LIBC_TEST=HERMETIC")

  # null check tests are death tests, remove from hermetic tests for now.
  if(LIBC_ADD_NULL_CHECKS)
    if(MSVC)
      list(REMOVE_ITEM compile_options "/DLIBC_ADD_NULL_CHECKS")
    else()
      list(REMOVE_ITEM compile_options "-DLIBC_ADD_NULL_CHECKS")
    endif()
  endif()

  # The GPU build requires overriding the default CMake triple and architecture.
  if(LIBC_TARGET_ARCHITECTURE_IS_AMDGPU)
    list(APPEND compile_options
         -Wno-multi-gpu -nogpulib -mcpu=${LIBC_GPU_TARGET_ARCHITECTURE} -flto
         -mcode-object-version=${LIBC_GPU_CODE_OBJECT_VERSION})
  elseif(LIBC_TARGET_ARCHITECTURE_IS_NVPTX)
    list(APPEND compile_options
         -Wno-multi-gpu --cuda-path=${LIBC_CUDA_ROOT}
         -nogpulib -march=${LIBC_GPU_TARGET_ARCHITECTURE} -fno-use-cxa-atexit)
  endif()

  set(${output_var} ${compile_options} PARENT_SCOPE)
endfunction()

# This is a helper function and not a build rule. It is to be used by the
# various test rules to generate the full list of object files
# recursively produced by "add_entrypoint_object" and "add_object_library"
# targets.
# Usage:
#   get_object_files_for_test(<result var>
#                             <skipped_entrypoints_var>
#                             <link_libraries_var>
#                             <target0> [<target1> ...])
#
#   The list of object files is collected in <result_var>.
#   If skipped entrypoints were found, then <skipped_entrypoints_var> is
#   set to a true value.
#   Any non-object library targets required by those dependencies are
#   collected in <link_libraries_var>.
#   targetN is either an "add_entrypoint_target" target or an
#   "add_object_library" target.
function(get_object_files_for_test result skipped_entrypoints_list
         link_libraries_result)
  set(object_files "")
  set(skipped_list "")
  set(link_libraries "")
  set(unchecked_list "${ARGN}")
  list(REMOVE_DUPLICATES unchecked_list)

  foreach(dep IN LISTS unchecked_list)
    if (NOT TARGET ${dep})
      # Skip tests with undefined dependencies.
      # Compiler-RT targets are added only if they are enabled. However, such targets may not be defined
      # at the time of the libc build. We should skip checking such targets.
      if (NOT ${dep} MATCHES "^RTScudo.*|^RTGwp.*")
        list(APPEND skipped_list ${dep})
      endif()
      continue()
    endif()
    get_target_property(aliased_target ${dep} "ALIASED_TARGET")
    if(aliased_target)
      # If the target is just an alias, switch to the real target.
      set(dep ${aliased_target})
    endif()

    get_target_property(dep_type ${dep} "TARGET_TYPE")
    if(NOT dep_type)
      # Imported and regular libraries participate in the final link, even
      # though they do not contribute object files directly.
      get_target_property(cmake_target_type ${dep} "TYPE")
      if(cmake_target_type STREQUAL "STATIC_LIBRARY"
         OR cmake_target_type STREQUAL "SHARED_LIBRARY"
         OR cmake_target_type STREQUAL "MODULE_LIBRARY"
         OR cmake_target_type STREQUAL "INTERFACE_LIBRARY"
         OR cmake_target_type STREQUAL "UNKNOWN_LIBRARY")
        list(APPEND link_libraries ${dep})
      endif()
      continue()
    endif()

    get_target_property(dep_checked ${dep} "CHECK_OBJ_FOR_TESTS")

    if(dep_checked)
      # Target full dependency has already been checked.  Just use the results.
      get_target_property(dep_obj ${dep} "OBJECT_FILES_FOR_TESTS")
      get_target_property(dep_skip ${dep} "SKIPPED_LIST_FOR_TESTS")
      get_target_property(dep_links ${dep} "LINK_LIBRARIES_FOR_TESTS")
    else()
      # Target full dependency hasn't been checked.  Recursively check its DEPS.
      # Mark as in-progress before recursing to break cycles.
      set_target_properties(${dep} PROPERTIES "CHECK_OBJ_FOR_TESTS" TRUE)
      set_target_properties(${dep} PROPERTIES "OBJECT_FILES_FOR_TESTS" "")
      set_target_properties(${dep} PROPERTIES "SKIPPED_LIST_FOR_TESTS" "")
      set_target_properties(${dep} PROPERTIES "LINK_LIBRARIES_FOR_TESTS" "")

      set(dep_obj "")
      set(dep_skip "")
      set(dep_links "")

      get_target_property(indirect_deps ${dep} "DEPS")
      get_object_files_for_test(dep_obj dep_skip dep_links ${indirect_deps})

      if(${dep_type} STREQUAL ${OBJECT_LIBRARY_TARGET_TYPE})
        get_target_property(dep_object_files ${dep} "OBJECT_FILES")
        if(dep_object_files)
          list(APPEND dep_obj ${dep_object_files})
        endif()
      elseif(${dep_type} STREQUAL ${ENTRYPOINT_OBJ_TARGET_TYPE})
        get_target_property(is_skipped ${dep} "SKIPPED")
        if(is_skipped)
          list(APPEND dep_skip ${dep})
          list(REMOVE_ITEM dep_obj ${dep})
        endif()
        get_target_property(object_file_raw ${dep} "OBJECT_FILE_RAW")
        if(object_file_raw)
          # TODO: Remove this once we stop suffixing the target with ".__internal__"
          if(fq_target_name STREQUAL "libc.test.include.issignaling_c_test.__unit__" OR fq_target_name STREQUAL "libc.test.include.iscanonical_c_test.__unit__")
            string(REPLACE ".__internal__" "" object_file_raw ${object_file_raw})
          endif()
          list(APPEND dep_obj ${object_file_raw})
        endif()
      endif()

      set_target_properties(${dep} PROPERTIES
        OBJECT_FILES_FOR_TESTS "${dep_obj}"
        SKIPPED_LIST_FOR_TESTS "${dep_skip}"
        LINK_LIBRARIES_FOR_TESTS "${dep_links}"
        CHECK_OBJ_FOR_TESTS "YES"
      )

    endif()

    list(APPEND object_files ${dep_obj})
    list(APPEND skipped_list ${dep_skip})
    list(APPEND link_libraries ${dep_links})

  endforeach(dep)

  list(REMOVE_DUPLICATES object_files)
  set(${result} ${object_files} PARENT_SCOPE)
  list(REMOVE_DUPLICATES skipped_list)
  set(${skipped_entrypoints_list} ${skipped_list} PARENT_SCOPE)
  list(REMOVE_DUPLICATES link_libraries)
  set(${link_libraries_result} ${link_libraries} PARENT_SCOPE)

endfunction(get_object_files_for_test)

function(get_link_options link_options)
  set(link_opts "")
  foreach(opt IN LISTS ARGN)
    if((NOT ${opt} MATCHES "^/D") AND (NOT ${opt} MATCHES "^-D"))
      list(APPEND link_opts ${opt})
    endif()
  endforeach()
  set(${link_options} "${link_opts}" PARENT_SCOPE)
endfunction()

# Rule to add a libc unittest.
# Usage
#    add_libc_unittest(
#      <target name>
#      SUITE <name of the suite this test belongs to>
#      SRCS  <list of .cpp files for the test>
#      HDRS  <list of .h files for the test>
#      DEPENDS <list of dependencies>
#      ENV <list of environment variables to set before running the test>
#      COMPILE_OPTIONS <list of special compile options for this target>
#      LINK_LIBRARIES <list of linking libraries for this target>
#    )
function(create_libc_unittest fq_target_name)
  if(NOT LLVM_INCLUDE_TESTS)
    return()
  endif()

  cmake_parse_arguments(
    "LIBC_UNITTEST"
    "NO_RUN_POSTBUILD;C_TEST;NO_ERRNO_SETTER_MATCHER" # Optional arguments
    "SUITE;CXX_STANDARD" # Single value arguments
    "SRCS;HDRS;DEPENDS;ENV;COMPILE_OPTIONS;LINK_LIBRARIES;FLAGS" # Multi-value arguments
    ${ARGN}
  )
  if(NOT LIBC_UNITTEST_SRCS)
    message(FATAL_ERROR "'add_libc_unittest' target requires a SRCS list of .cpp "
                        "files.")
  endif()
  if(NOT LIBC_UNITTEST_DEPENDS)
    message(FATAL_ERROR "'add_libc_unittest' target requires a DEPENDS list of "
                        "'add_entrypoint_object' targets.")
  endif()

  get_fq_deps_list(fq_deps_list ${LIBC_UNITTEST_DEPENDS})

  if(NOT LIBC_UNITTEST_C_TEST)
    if(NOT LIBC_UNITTEST_NO_ERRNO_SETTER_MATCHER)
      list(APPEND fq_deps_list libc.src.__support.StringUtil.error_to_string
                               libc.test.UnitTest.ErrnoSetterMatcher)
    endif()
    # LibcTest.unit calls LIBC_NAMESPACE::clock() when TARGET_SUPPORTS_CLOCK
    # is defined, so every unit test needs the clock object linked in.
    if(libc.src.time.clock IN_LIST TARGET_LLVMLIBC_ENTRYPOINTS)
      list(APPEND fq_deps_list libc.src.time.clock)
    endif()
  endif()

  # Freestanding unit-test link: pull the same auxiliary deps as hermetic
  # tests so the resulting binary has everything it needs without relying on
  # a host libc. Mirrors the list in add_libc_hermetic.
  if(LIBC_UNIT_TEST_LINK_FREESTANDING AND NOT LIBC_UNITTEST_C_TEST)
    list(APPEND fq_deps_list
        libc.src.__support.StringUtil.error_to_string
        libc.src.stdio.vsnprintf
        libc.src.stdlib.aligned_alloc
        libc.src.string.memcmp
        libc.src.string.memcpy
        libc.src.string.memmove
        libc.src.string.memset
        libc.src.string.strcmp
        libc.src.string.strlen
        libc.src.strings.bcmp
        libc.src.strings.bzero
    )
    # HermeticTestUtils ships an extern-C syscall() shim on NT-POSIX; pull
    # the fixed-arg entrypoint it forwards to so the shim resolves at link.
    if(TARGET libc.src.unistd.__llvm_libc_syscall)
      list(APPEND fq_deps_list libc.src.unistd.__llvm_libc_syscall)
    endif()
    if(TARGET libc.startup.${LIBC_TARGET_OS}.hermetic_runtime)
      list(APPEND fq_deps_list libc.startup.${LIBC_TARGET_OS}.hermetic_runtime)
    endif()
    if(TARGET libc.startup.${LIBC_TARGET_OS}.crt1)
      list(APPEND fq_deps_list libc.startup.${LIBC_TARGET_OS}.crt1)
    endif()
    if(TARGET libc.src.compiler.__stack_chk_fail)
      list(APPEND fq_deps_list libc.src.compiler.__stack_chk_fail)
    elseif(TARGET libc.src.compiler.generic.__stack_chk_fail)
      list(APPEND fq_deps_list libc.src.compiler.generic.__stack_chk_fail)
    endif()
    # Death-test executors call LIBC_NAMESPACE:: POSIX primitives directly;
    # every unit test links LibcDeathTestExecutors.unit, so every test must
    # carry these entrypoints to resolve them.
    if(TARGET libc.src.unistd.fork)
      list(APPEND fq_deps_list
        libc.src.poll.poll
        libc.src.signal.kill
        libc.src.stdlib._Exit
        libc.src.string.strsignal
        libc.src.sys.wait.waitpid
        libc.src.unistd.close
        libc.src.unistd.fork
        libc.src.unistd.pipe
      )
    endif()
  endif()

  list(REMOVE_DUPLICATES fq_deps_list)

  _get_common_test_compile_options(compile_options "${LIBC_UNITTEST_C_TEST}"
                                   "${LIBC_UNITTEST_FLAGS}")
  libc_add_definition(compile_options "LIBC_TEST=UNIT")

  get_link_options(link_options
                   ${compile_options}
                   ${LIBC_LINK_OPTIONS_DEFAULT}
                   ${LIBC_TEST_LINK_OPTIONS_DEFAULT}
  )
  # Freestanding link: suppress the driver's default libc/startfiles/libc++
  # injection. __llvm_libc_* symbols then resolve uniquely from the libc
  # __internal__ objects collected via DEPS, and extern-C compiler-runtime
  # calls (memcpy/malloc/etc.) resolve from LibcHermeticTestSupport.unit
  # appended to link_libraries below.
  if(LIBC_UNIT_TEST_LINK_FREESTANDING AND NOT LIBC_UNITTEST_C_TEST)
    if(LIBC_CC_SUPPORTS_NOSTDLIBPP)
      list(APPEND link_options -nolibc -nostartfiles -nostdlib++)
    else()
      list(APPEND link_options -nolibc -nostartfiles -nostdlib)
    endif()
    if(LIBC_TARGET_OS_IS_WINDOWS)
      list(APPEND link_options -unwindlib=none)
    endif()
  endif()
  list(APPEND compile_options ${LIBC_UNITTEST_COMPILE_OPTIONS})

  if(SHOW_INTERMEDIATE_OBJECTS)
    message(STATUS "Adding unit test ${fq_target_name}")
    if(${SHOW_INTERMEDIATE_OBJECTS} STREQUAL "DEPS")
      foreach(dep IN LISTS LIBC_UNITTEST_DEPENDS)
        message(STATUS "  ${fq_target_name} depends on ${dep}")
      endforeach()
    endif()
  endif()

  get_object_files_for_test(
      link_object_files skipped_entrypoints_list inherited_link_libraries
      ${fq_deps_list})
  list(REMOVE_DUPLICATES link_object_files)
  if(skipped_entrypoints_list)
    # If a test is OS/target machine independent, it has to be skipped if the
    # OS/target machine combination does not provide any dependent entrypoints.
    # If a test is OS/target machine specific, then such a test will live is a
    # OS/target machine specific directory and will be skipped at the directory
    # level if required.
    #
    # There can potentially be a setup like this: A unittest is setup for a
    # OS/target machine independent object library, which in turn depends on a
    # machine specific object library. Such a test would be testing internals of
    # the libc and it is assumed that they will be rare in practice. So, they
    # can be skipped in the corresponding CMake files using platform specific
    # logic. This pattern is followed in the startup tests for example.
    #
    # Another pattern that is present currently is to detect machine
    # capabilities and add entrypoints and tests accordingly. That approach is
    # much lower level approach and is independent of the kind of skipping that
    # is happening here at the entrypoint level.
    if(LIBC_CMAKE_VERBOSE_LOGGING)
      set(msg "Skipping unittest ${fq_target_name} as it has missing deps: "
              "${skipped_entrypoints_list}.")
      message(STATUS ${msg})
    endif()
    return()
  endif()

  if(LIBC_UNITTEST_NO_RUN_POSTBUILD)
    set(fq_build_target_name ${fq_target_name})
  else()
    set(fq_build_target_name ${fq_target_name}.__build__)
  endif()

  add_executable(
    ${fq_build_target_name}
    EXCLUDE_FROM_ALL
    ${LIBC_UNITTEST_SRCS}
    ${LIBC_UNITTEST_HDRS}
  )
  target_include_directories(${fq_build_target_name} SYSTEM PRIVATE ${LIBC_INCLUDE_DIR})
  target_include_directories(${fq_build_target_name} PRIVATE ${LIBC_SOURCE_DIR})
  target_include_directories(${fq_build_target_name} PRIVATE ${LIBC_BUILD_DIR})
  target_compile_options(${fq_build_target_name} PRIVATE ${compile_options})
  target_link_options(${fq_build_target_name} PRIVATE ${link_options})

  if(NOT LIBC_UNITTEST_CXX_STANDARD)
    set(LIBC_UNITTEST_CXX_STANDARD ${CMAKE_CXX_STANDARD})
  endif()
  set_target_properties(
    ${fq_build_target_name}
    PROPERTIES
      CXX_STANDARD ${LIBC_UNITTEST_CXX_STANDARD}
  )

  # Freestanding-unit link packs the dep objects into a static archive so
  # archive-extraction semantics resolve NAME-colliding entrypoints (e.g. the
  # default libc.src.string.memcmp vs. a variant libc.src.string.memcmp_x86_64
  # _opt_sse2 both pulled in by add_libc_multi_impl_test). User-supplied DEPS
  # sit first in link_object_files, so the variant member resolves the
  # unresolved LIBC_NAMESPACE::memcmp and the default member is never pulled.
  # Matches the pattern add_libc_hermetic already uses.
  if(LIBC_UNIT_TEST_LINK_FREESTANDING AND NOT LIBC_UNITTEST_C_TEST)
    add_library(
      ${fq_target_name}.__libc__
      STATIC
      EXCLUDE_FROM_ALL
      ${link_object_files}
    )
    set_target_properties(${fq_target_name}.__libc__
        PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_target_properties(${fq_target_name}.__libc__
        PROPERTIES ARCHIVE_OUTPUT_NAME ${fq_target_name}.libc)
    if(inherited_link_libraries)
      target_link_libraries(${fq_target_name}.__libc__
                            PUBLIC ${inherited_link_libraries})
    endif()
    set(link_libraries ${fq_target_name}.__libc__)
  else()
    set(link_libraries ${link_object_files} ${inherited_link_libraries})
  endif()
  # Test object files will depend on LINK_LIBRARIES passed down from `add_fp_unittest`
  foreach(lib IN LISTS LIBC_UNITTEST_LINK_LIBRARIES)
    if(TARGET ${lib}.unit)
      list(APPEND link_libraries ${lib}.unit)
    else()
      list(APPEND link_libraries ${lib})
    endif()
  endforeach()

  set_target_properties(${fq_build_target_name}
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_dependencies(
    ${fq_build_target_name}
    ${fq_deps_list}
  )

  # LibcUnitTest should not depend on anything in LINK_LIBRARIES.
  if(NOT LIBC_UNITTEST_C_TEST)
    list(APPEND link_libraries LibcDeathTestExecutors.unit LibcTest.unit)
  endif()

  # Freestanding link: provide extern-C shims for malloc/memcpy/etc. that the
  # compiler may emit for codegen, and — on Windows — the NT kernel import
  # libs the driver would otherwise transitively pull via c.lib.
  if(LIBC_UNIT_TEST_LINK_FREESTANDING AND NOT LIBC_UNITTEST_C_TEST)
    list(APPEND link_libraries LibcHermeticTestSupport.unit)
    if(LIBC_TARGET_OS_IS_WINDOWS)
      foreach(_imp windows_ntdll windows_bcryptprimitives windows_sspicli)
        if(TARGET ${_imp})
          list(APPEND link_libraries ${_imp})
        endif()
      endforeach()
    endif()
  endif()

  target_link_libraries(${fq_build_target_name} PRIVATE ${link_libraries})

  if(NOT LIBC_UNITTEST_NO_RUN_POSTBUILD)
    add_custom_target(
      ${fq_target_name}
      COMMAND ${LIBC_UNITTEST_ENV} ${CMAKE_CROSSCOMPILING_EMULATOR} ${CMAKE_CURRENT_BINARY_DIR}/${fq_build_target_name}
      COMMENT "Running unit test ${fq_target_name}"
      DEPENDS ${fq_build_target_name}
    )
  endif()

  # The SUITE variable can be used to group tests into a custom target. 
  # If a target named ${LIBC_UNITTEST_SUITE}-build exists, we add the 
  # test executable to it as a dependency. This allows building the 
  # test binaries for Lit without triggering their execution.
  if(LIBC_UNITTEST_SUITE)
    add_dependencies(${LIBC_UNITTEST_SUITE} ${fq_target_name})
    if(TARGET ${LIBC_UNITTEST_SUITE}-build)
      add_dependencies(${LIBC_UNITTEST_SUITE}-build ${fq_build_target_name})
    endif()
  endif()
  add_dependencies(libc-unit-tests ${fq_target_name})
  if(TARGET libc-unit-tests-build)
    add_dependencies(libc-unit-tests-build ${fq_build_target_name})
  endif()
endfunction(create_libc_unittest)

function(add_libc_unittest target_name)
  add_target_with_flags(
    ${target_name}
    CREATE_TARGET create_libc_unittest
    ${ARGN}
  )
endfunction(add_libc_unittest)

function(add_libc_exhaustive_testsuite suite_name)
  add_custom_target(${suite_name})
  add_dependencies(exhaustive-check-libc ${suite_name})
endfunction(add_libc_exhaustive_testsuite)

function(add_libc_long_running_testsuite suite_name)
  add_custom_target(${suite_name})
  add_dependencies(libc-long-running-tests ${suite_name})
endfunction(add_libc_long_running_testsuite)

# Rule to add a fuzzer test.
# Usage
#    add_libc_fuzzer(
#      <target name>
#      SRCS  <list of .cpp files for the test>
#      HDRS  <list of .h files for the test>
#      DEPENDS <list of dependencies>
#    )
function(add_libc_fuzzer target_name)
  cmake_parse_arguments(
    "LIBC_FUZZER"
    "NEED_MPFR" # Optional arguments
    "" # Single value arguments
    "SRCS;HDRS;DEPENDS;COMPILE_OPTIONS" # Multi-value arguments
    ${ARGN}
  )
  if(NOT LIBC_FUZZER_SRCS)
    message(FATAL_ERROR "'add_libc_fuzzer' target requires a SRCS list of .cpp "
                        "files.")
  endif()
  if(NOT LIBC_FUZZER_DEPENDS)
    message(FATAL_ERROR "'add_libc_fuzzer' target requires a DEPENDS list of "
                        "'add_entrypoint_object' targets.")
  endif()

  list(APPEND LIBC_FUZZER_LINK_LIBRARIES "")
  if(LIBC_FUZZER_NEED_MPFR)
    if(NOT LIBC_TESTS_CAN_USE_MPFR)
      message(VERBOSE "Fuzz test ${name} will be skipped as MPFR library is not available.")
      return()
    endif()
    list(APPEND LIBC_FUZZER_LINK_LIBRARIES mpfr gmp)
  endif()


  get_fq_target_name(${target_name} fq_target_name)
  get_fq_deps_list(fq_deps_list ${LIBC_FUZZER_DEPENDS})
  get_object_files_for_test(
      link_object_files skipped_entrypoints_list inherited_link_libraries
      ${fq_deps_list})
  if(skipped_entrypoints_list)
    if(LIBC_CMAKE_VERBOSE_LOGGING)
      set(msg "Skipping fuzzer target ${fq_target_name} as it has missing deps: "
              "${skipped_entrypoints_list}.")
      message(STATUS ${msg})
    endif()
    add_custom_target(${fq_target_name})

    # A post build custom command is used to avoid running the command always.
    add_custom_command(
      TARGET ${fq_target_name}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E echo ${msg}
    )
    return()
  endif()

  add_executable(
    ${fq_target_name}
    EXCLUDE_FROM_ALL
    ${LIBC_FUZZER_SRCS}
    ${LIBC_FUZZER_HDRS}
  )
  target_include_directories(${fq_target_name} SYSTEM PRIVATE ${LIBC_INCLUDE_DIR})
  target_include_directories(${fq_target_name} PRIVATE ${LIBC_SOURCE_DIR})

  target_link_libraries(${fq_target_name} PRIVATE
    ${link_object_files}
    ${inherited_link_libraries}
    ${LIBC_FUZZER_LINK_LIBRARIES}
  )

  set_target_properties(${fq_target_name}
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_dependencies(
    ${fq_target_name}
    ${fq_deps_list}
  )
  add_dependencies(libc-fuzzer ${fq_target_name})

  target_compile_options(${fq_target_name}
    PRIVATE
    ${LIBC_FUZZER_COMPILE_OPTIONS})

endfunction(add_libc_fuzzer)

# Get libgcc_s to be used in hermetic and integration tests.
if(NOT MSVC AND NOT LIBC_CC_SUPPORTS_NOSTDLIBPP)
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgcc_s.so.1
                  OUTPUT_VARIABLE LIBGCC_S_LOCATION)
  string(STRIP ${LIBGCC_S_LOCATION} LIBGCC_S_LOCATION)
endif()

# DEPRECATED: Use add_hermetic_test instead.
#
# Rule to add an integration test. An integration test is like a unit test
# but does not use the system libc. Not even the startup objects from the
# system libc are linked in to the final executable. The final exe is fully
# statically linked. The libc that the final exe links to consists of only
# the object files of the DEPENDS targets.
#
# Usage:
#   add_integration_test(
#     <target name>
#     SUITE <the suite to which the test should belong>
#     SRCS <src1.cpp> [src2.cpp ...]
#     HDRS [hdr1.cpp ...]
#     DEPENDS <list of entrypoint or other object targets>
#     ARGS <list of command line arguments to be passed to the test>
#     ENV <list of environment variables to set before running the test>
#     COMPILE_OPTIONS <list of special compile options for this target>
#   )
#
# The DEPENDS list can be empty. If not empty, it should be a list of
# targets added with add_entrypoint_object or add_object_library.
function(add_integration_test test_name)
  get_fq_target_name(${test_name} fq_target_name)
  set(supported_targets gpu linux)
  if(NOT (${LIBC_TARGET_OS} IN_LIST supported_targets))
    message(STATUS "Skipping ${fq_target_name} as it is not available on ${LIBC_TARGET_OS}.")
    return()
  endif()
  cmake_parse_arguments(
    "INTEGRATION_TEST"
    "" # No optional arguments
    "SUITE" # Single value arguments
    "SRCS;HDRS;DEPENDS;ARGS;ENV;COMPILE_OPTIONS;LOADER_ARGS" # Multi-value arguments
    ${ARGN}
  )

  if(NOT INTEGRATION_TEST_SUITE)
    message(FATAL_ERROR "SUITE not specified for ${fq_target_name}")
  endif()
  if(NOT INTEGRATION_TEST_SRCS)
    message(FATAL_ERROR "The SRCS list for add_integration_test is missing.")
  endif()
  if(NOT LLVM_LIBC_FULL_BUILD AND NOT TARGET libc.startup.${LIBC_TARGET_OS}.crt1)
    message(FATAL_ERROR "The 'crt1' target for the integration test is missing.")
  endif()

  get_fq_target_name(${test_name}.libc fq_libc_target_name)

  get_fq_deps_list(fq_deps_list ${INTEGRATION_TEST_DEPENDS})
  list(APPEND fq_deps_list
      # All integration tests use the operating system's startup object with the
      # integration test object and need to inherit the same dependencies.
      libc.startup.${LIBC_TARGET_OS}.crt1
      libc.test.IntegrationTest.test
      # We always add the memory functions objects. This is because the
      # compiler's codegen can emit calls to the C memory functions.
      libc.src.string.memcmp
      libc.src.string.memcpy
      libc.src.string.memmove
      libc.src.string.memset
      libc.src.strings.bcmp
      libc.src.strings.bzero
  )

  if(TARGET libc.src.compiler.__stack_chk_fail)
    # __stack_chk_fail should always be included if supported to allow building
    # libc with the stack protector enabled.
    list(APPEND fq_deps_list libc.src.compiler.__stack_chk_fail)
  elseif(TARGET libc.src.compiler.generic.__stack_chk_fail)
    list(APPEND fq_deps_list libc.src.compiler.generic.__stack_chk_fail)
  endif()

  list(REMOVE_DUPLICATES fq_deps_list)

  # TODO: Instead of gathering internal object files from entrypoints,
  # collect the object files with public names of entrypoints.
  get_object_files_for_test(
      link_object_files skipped_entrypoints_list inherited_link_libraries
      ${fq_deps_list})
  if(skipped_entrypoints_list)
    if(LIBC_CMAKE_VERBOSE_LOGGING)
      set(msg "Skipping integration test ${fq_target_name} as it has missing deps: "
              "${skipped_entrypoints_list}.")
      message(STATUS ${msg})
    endif()
    return()
  endif()
  list(REMOVE_DUPLICATES link_object_files)

  # Make a library of all deps
  add_library(
    ${fq_target_name}.__libc__
    STATIC
    EXCLUDE_FROM_ALL
    ${link_object_files}
  )
  set_target_properties(${fq_target_name}.__libc__
      PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_target_properties(${fq_target_name}.__libc__
      PROPERTIES ARCHIVE_OUTPUT_NAME ${fq_target_name}.libc)
  if(inherited_link_libraries)
    target_link_libraries(${fq_target_name}.__libc__
                          PUBLIC ${inherited_link_libraries})
  endif()

  set(fq_build_target_name ${fq_target_name}.__build__)
  add_executable(
    ${fq_build_target_name}
    EXCLUDE_FROM_ALL
    ${INTEGRATION_TEST_SRCS}
    ${INTEGRATION_TEST_HDRS}
  )
  set_target_properties(${fq_build_target_name}
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  target_include_directories(${fq_build_target_name} SYSTEM PRIVATE ${LIBC_INCLUDE_DIR})
  target_include_directories(${fq_build_target_name} PRIVATE ${LIBC_SOURCE_DIR})

  _get_hermetic_test_compile_options(compile_options "")
  target_compile_options(${fq_build_target_name} PRIVATE
                         ${compile_options} ${INTEGRATION_TEST_COMPILE_OPTIONS})

  set(compiler_runtime "")

  if(LIBC_TARGET_ARCHITECTURE_IS_AMDGPU)
    target_link_options(${fq_build_target_name} PRIVATE
      ${LIBC_COMPILE_OPTIONS_DEFAULT} ${INTEGRATION_TEST_COMPILE_OPTIONS}
      -Wno-multi-gpu -mcpu=${LIBC_GPU_TARGET_ARCHITECTURE} -flto -nostdlib -static
      "-Wl,-mllvm,-amdhsa-code-object-version=${LIBC_GPU_CODE_OBJECT_VERSION}")
  elseif(LIBC_TARGET_ARCHITECTURE_IS_NVPTX)
    target_link_options(${fq_build_target_name} PRIVATE
      ${LIBC_COMPILE_OPTIONS_DEFAULT} ${INTEGRATION_TEST_COMPILE_OPTIONS}
      "-Wl,--suppress-stack-size-warning" -Wno-multi-gpu
      "-Wl,-mllvm,-nvptx-emit-init-fini-kernel"
      -march=${LIBC_GPU_TARGET_ARCHITECTURE} -nostdlib -static
      "--cuda-path=${LIBC_CUDA_ROOT}")
  elseif(LIBC_CC_SUPPORTS_NOSTDLIBPP)
    set(link_options
      -nolibc
      -nostartfiles
      -nostdlib++
      -static
      ${LIBC_LINK_OPTIONS_DEFAULT}
      ${LIBC_TEST_LINK_OPTIONS_DEFAULT}
    )
    target_link_options(${fq_build_target_name} PRIVATE ${link_options})
  else()
    # Older version of gcc does not support `nostdlib++` flag.  We use
    # `nostdlib` and link against libgcc_s, which cannot be linked statically.
    set(link_options
      -nolibc
      -nostartfiles
      -nostdlib
      ${LIBC_LINK_OPTIONS_DEFAULT}
      ${LIBC_TEST_LINK_OPTIONS_DEFAULT}
    )
    target_link_options(${fq_build_target_name} PRIVATE ${link_options})
    list(APPEND compiler_runtime ${LIBGCC_S_LOCATION})
  endif()
  target_link_libraries(
    ${fq_build_target_name}
    libc.startup.${LIBC_TARGET_OS}.crt1
    libc.test.IntegrationTest.test
    ${fq_target_name}.__libc__
    ${compiler_runtime}
  )
  add_dependencies(${fq_build_target_name}
                   libc.test.IntegrationTest.test
                   ${INTEGRATION_TEST_DEPENDS})

  # We have to use a separate var to store the command as a list because
  # the COMMAND option of `add_custom_target` cannot handle empty vars in the
  # command. For example, if INTEGRATION_TEST_ENV is empty, the actual
  # command also will not run. So, we use this list and tell `add_custom_target`
  # to expand the list (by including the option COMMAND_EXPAND_LISTS). This
  # makes `add_custom_target` construct the correct command and execute it.
  set(test_cmd
      ${INTEGRATION_TEST_ENV}
      $<$<BOOL:${LIBC_TARGET_ARCHITECTURE_IS_NVPTX}>:LIBOMPTARGET_STACK_SIZE=3072>
      ${CMAKE_CROSSCOMPILING_EMULATOR}
      ${INTEGRATION_TEST_LOADER_ARGS}
      $<TARGET_FILE:${fq_build_target_name}> ${INTEGRATION_TEST_ARGS})
  # Generate a sidecar .params file alongside the executable for any test that
  # requires specific command-line arguments or environment variables.  The
  # LibcTest lit format reads this file at test time.  Format: one arg per line,
  # a "---" separator, then one KEY=VALUE env entry per line.
  if(INTEGRATION_TEST_ARGS OR INTEGRATION_TEST_ENV)
    set(_params_content "")
    foreach(_arg IN LISTS INTEGRATION_TEST_ARGS)
      string(APPEND _params_content "${_arg}\n")
    endforeach()
    string(APPEND _params_content "---\n")
    foreach(_env_entry IN LISTS INTEGRATION_TEST_ENV)
      string(APPEND _params_content "${_env_entry}\n")
    endforeach()
    file(GENERATE
      OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/${fq_build_target_name}.params"
      CONTENT "${_params_content}"
    )
  endif()

  add_custom_target(
    ${fq_target_name}
    COMMAND ${test_cmd}
    COMMAND_EXPAND_LISTS
    COMMENT "Running integration test ${fq_target_name}"
  )
  if(INTEGRATION_TEST_SUITE)
    add_dependencies(${INTEGRATION_TEST_SUITE} ${fq_target_name})
    # If a target named ${INTEGRATION_TEST_SUITE}-build exists, we add the 
    # test executable to it as a dependency. This allows building the 
    # test binaries for Lit without triggering their execution.
    if(TARGET ${INTEGRATION_TEST_SUITE}-build)
      add_dependencies(${INTEGRATION_TEST_SUITE}-build ${fq_build_target_name})
    endif()
  endif()
  if(TARGET libc-integration-tests-build)
    add_dependencies(libc-integration-tests-build ${fq_build_target_name})
  endif()
endfunction(add_integration_test)

# Rule to add a hermetic program. A hermetic program is one whose executable is fully
# statically linked and consists of pieces drawn only from LLVM's libc. Nothing,
# including the startup objects, come from the system libc.
#
# For the GPU, these can be either tests or benchmarks, depending on the value
# of the LINK_LIBRARIES arg.
#
# Usage:
#   add_libc_hermetic(
#     <target name>
#     SUITE <the suite to which the test should belong>
#     SRCS <src1.cpp> [src2.cpp ...]
#     HDRS [hdr1.cpp ...]
#     DEPENDS <list of entrypoint or other object targets>
#     ARGS <list of command line arguments to be passed to the test>
#     ENV <list of environment variables to set before running the test>
#     COMPILE_OPTIONS <list of special compile options for the test>
#     LINK_LIBRARIES <list of linking libraries for this target>
#     LOADER_ARGS <list of special args to loaders (like the GPU loader)>
#   )
function(add_libc_hermetic test_name)
  if(NOT TARGET libc.startup.${LIBC_TARGET_OS}.crt1)
    message(VERBOSE "Skipping ${fq_target_name} as it is not available on ${LIBC_TARGET_OS}.")
    return()
  endif()
  cmake_parse_arguments(
    "HERMETIC_TEST"
    "IS_GPU_BENCHMARK;NO_RUN_POSTBUILD" # Optional arguments
    "SUITE;CXX_STANDARD" # Single value arguments
    "SRCS;HDRS;DEPENDS;ARGS;ENV;COMPILE_OPTIONS;LINK_LIBRARIES;LOADER_ARGS" # Multi-value arguments
    ${ARGN}
  )

  if(NOT HERMETIC_TEST_SUITE)
    message(FATAL_ERROR "SUITE not specified for ${fq_target_name}")
  endif()
  if(NOT HERMETIC_TEST_SRCS)
    message(FATAL_ERROR "The SRCS list for add_integration_test is missing.")
  endif()

  get_fq_target_name(${test_name} fq_target_name)
  get_fq_target_name(${test_name}.libc fq_libc_target_name)

  get_fq_deps_list(fq_deps_list ${HERMETIC_TEST_DEPENDS})
  list(APPEND fq_deps_list
      # Hermetic tests use the platform's startup object. So, their deps also
      # have to be collected.
      libc.startup.${LIBC_TARGET_OS}.crt1
      # We always add the memory functions objects. This is because the
      # compiler's codegen can emit calls to the C memory functions.
      libc.src.__support.StringUtil.error_to_string
      libc.src.stdio.vsnprintf
      libc.src.stdlib.aligned_alloc
      libc.src.string.memcmp
      libc.src.string.memcpy
      libc.src.string.memmove
      libc.src.string.memset
      libc.src.string.strcmp
      libc.src.string.strlen
      libc.src.strings.bcmp
      libc.src.strings.bzero
  )

  # Windows hermetic exes need the C++ itanium ABI floor (__cxa_guard_*,
  # __cxa_finalize, exit/atexit, setjmp). Pull these via one aggregate so
  # tests don't re-declare them per bench.
  if(LIBC_TARGET_OS_IS_WINDOWS AND
     TARGET libc.startup.${LIBC_TARGET_OS}.hermetic_runtime)
    list(APPEND fq_deps_list libc.startup.${LIBC_TARGET_OS}.hermetic_runtime)
  endif()

  if(TARGET libc.src.compiler.__stack_chk_fail)
    # __stack_chk_fail should always be included if supported to allow building
    # libc with the stack protector enabled.
    list(APPEND fq_deps_list libc.src.compiler.__stack_chk_fail)
  elseif(TARGET libc.src.compiler.generic.__stack_chk_fail)
    list(APPEND fq_deps_list libc.src.compiler.generic.__stack_chk_fail)
  endif()

  if(libc.src.time.clock IN_LIST TARGET_LLVMLIBC_ENTRYPOINTS)
    # We will link in the 'clock' implementation if it exists for test timing.
    list(APPEND fq_deps_list libc.src.time.clock)
  endif()

  list(REMOVE_DUPLICATES fq_deps_list)

  # TODO: Instead of gathering internal object files from entrypoints,
  # collect the object files with public names of entrypoints.
  get_object_files_for_test(
      link_object_files skipped_entrypoints_list inherited_link_libraries
      ${fq_deps_list})
  if(skipped_entrypoints_list)
    if(LIBC_CMAKE_VERBOSE_LOGGING)
      set(msg "Skipping hermetic test ${fq_target_name} as it has missing deps: "
              "${skipped_entrypoints_list}.")
      message(STATUS ${msg})
    endif()
    return()
  endif()
  list(REMOVE_DUPLICATES link_object_files)

  # Make a library of all deps
  add_library(
    ${fq_target_name}.__libc__
    STATIC
    EXCLUDE_FROM_ALL
    ${link_object_files}
  )
  set_target_properties(${fq_target_name}.__libc__
      PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_target_properties(${fq_target_name}.__libc__
      PROPERTIES ARCHIVE_OUTPUT_NAME ${fq_target_name}.libc)
  if(inherited_link_libraries)
    target_link_libraries(${fq_target_name}.__libc__
                          PUBLIC ${inherited_link_libraries})
  endif()

  if(HERMETIC_TEST_NO_RUN_POSTBUILD)
    set(fq_build_target_name ${fq_target_name})
  else()
    set(fq_build_target_name ${fq_target_name}.__build__)
  endif()

  add_executable(
    ${fq_build_target_name}
    EXCLUDE_FROM_ALL
    ${HERMETIC_TEST_SRCS}
    ${HERMETIC_TEST_HDRS}
  )

  if(NOT HERMETIC_TEST_CXX_STANDARD)
    set(HERMETIC_TEST_CXX_STANDARD ${CMAKE_CXX_STANDARD})
  endif()
  set_target_properties(${fq_build_target_name}
    PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      CXX_STANDARD ${HERMETIC_TEST_CXX_STANDARD}
  )

  target_include_directories(${fq_build_target_name} SYSTEM PRIVATE ${LIBC_INCLUDE_DIR})
  target_include_directories(${fq_build_target_name} PRIVATE ${LIBC_SOURCE_DIR})
  _get_hermetic_test_compile_options(compile_options "")
  target_compile_options(${fq_build_target_name} PRIVATE
                         ${compile_options}
                         ${HERMETIC_TEST_COMPILE_OPTIONS})

  set(link_libraries "")
  set(compiler_runtime "")
  foreach(lib IN LISTS HERMETIC_TEST_LINK_LIBRARIES)
    if(TARGET ${lib}.hermetic)
      list(APPEND link_libraries ${lib}.hermetic)
    else()
      list(APPEND link_libraries ${lib})
    endif()
  endforeach()

  if(LIBC_TARGET_ARCHITECTURE_IS_AMDGPU)
    target_link_options(${fq_build_target_name} PRIVATE
      ${LIBC_COMPILE_OPTIONS_DEFAULT} -Wno-multi-gpu
      -mcpu=${LIBC_GPU_TARGET_ARCHITECTURE} -flto -nostdlib -static
      "-Wl,-mllvm,-amdhsa-code-object-version=${LIBC_GPU_CODE_OBJECT_VERSION}")
  elseif(LIBC_TARGET_ARCHITECTURE_IS_NVPTX)
    target_link_options(${fq_build_target_name} PRIVATE
      ${LIBC_COMPILE_OPTIONS_DEFAULT} -Wno-multi-gpu
      "-Wl,--suppress-stack-size-warning"
      "-Wl,-mllvm,-nvptx-emit-init-fini-kernel"
      -march=${LIBC_GPU_TARGET_ARCHITECTURE} -nostdlib -static
      "--cuda-path=${LIBC_CUDA_ROOT}")
  elseif(LIBC_TARGET_OS_IS_WINDOWS)
    # PE/COFF hermetic link. -static is not meaningful for lld-link; the
    # absence of c.lib/libc.lib plus -nolibc is what makes this hermetic.
    # -nostartfiles suppresses crt1.obj / crt_do_start.obj auto-injection —
    # libc.startup.windows.crt1 supplies them via DEPS instead.
    # -unwindlib=none prevents the driver from pulling unwind.lib (not built
    # yet during libc stage-2).
    set(link_options
      -nolibc
      -nostartfiles
      -nostdlib++
      -unwindlib=none
      ${LIBC_LINK_OPTIONS_DEFAULT}
      ${LIBC_TEST_LINK_OPTIONS_DEFAULT}
    )
    target_link_options(${fq_build_target_name} PRIVATE ${link_options})
  elseif(LIBC_CC_SUPPORTS_NOSTDLIBPP)
    set(link_options
      -nolibc
      -nostartfiles
      -nostdlib++
      -static
      ${LIBC_LINK_OPTIONS_DEFAULT}
      ${LIBC_TEST_LINK_OPTIONS_DEFAULT}
    )
    target_link_options(${fq_build_target_name} PRIVATE ${link_options})
  else()
    # Older version of gcc does not support `nostdlib++` flag.  We use
    # `nostdlib` and link against libgcc_s, which cannot be linked statically.
    set(link_options
      -nolibc
      -nostartfiles
      -nostdlib
      ${LIBC_LINK_OPTIONS_DEFAULT}
      ${LIBC_TEST_LINK_OPTIONS_DEFAULT}
    )
    target_link_options(${fq_build_target_name} PRIVATE ${link_options})
    list(APPEND compiler_runtime ${LIBGCC_S_LOCATION})
  endif()

  # Windows hermetic binaries must link NT kernel import libraries directly —
  # every libc primitive eventually dispatches into ntdll; bcryptprimitives is
  # needed by security_cookie init; sspicli is needed for SID/token queries.
  # The driver would normally add these via c.lib's transitive deps, but under
  # -nolibc we have to be explicit.
  set(_hermetic_windows_system_libs "")
  if(LIBC_TARGET_OS_IS_WINDOWS)
    foreach(_imp windows_ntdll windows_bcryptprimitives windows_sspicli)
      if(TARGET ${_imp})
        list(APPEND _hermetic_windows_system_libs ${_imp})
      endif()
    endforeach()
  endif()
  target_link_libraries(
    ${fq_build_target_name}
    PRIVATE
      libc.startup.${LIBC_TARGET_OS}.crt1
      ${link_libraries}
      LibcHermeticTestSupport.hermetic
      ${fq_target_name}.__libc__
      ${_hermetic_windows_system_libs}
      ${compiler_runtime}
    )
  add_dependencies(${fq_build_target_name}
                   LibcTest.hermetic
                   libc.test.UnitTest.ErrnoSetterMatcher
                   ${fq_deps_list})
  # TODO: currently the dependency chain is broken such that getauxval cannot properly
  # propagate to hermetic tests. This is a temporary workaround.
  if (LIBC_TARGET_ARCHITECTURE_IS_AARCH64 AND NOT(LIBC_TARGET_OS_IS_BAREMETAL))
    target_link_libraries(
      ${fq_build_target_name}
      PRIVATE
        libc.src.sys.auxv.getauxval
    )
  endif()

  if(NOT HERMETIC_TEST_NO_RUN_POSTBUILD)
    if (LIBC_TEST_CMD)
      # In the form of "<command> binary=@BINARY@", e.g. "qemu-system-arm -loader$<COMMA>file=@BINARY@"
      string(REPLACE "@BINARY@" "$<TARGET_FILE:${fq_build_target_name}>" test_cmd_parsed ${LIBC_TEST_CMD})
      string(REPLACE " " ";" test_cmd "${test_cmd_parsed}")
    else()
      set(test_cmd ${HERMETIC_TEST_ENV}
        $<$<BOOL:${LIBC_TARGET_ARCHITECTURE_IS_NVPTX}>:LIBOMPTARGET_STACK_SIZE=3072>
        ${CMAKE_CROSSCOMPILING_EMULATOR} ${HERMETIC_TEST_LOADER_ARGS}
        $<TARGET_FILE:${fq_build_target_name}> ${HERMETIC_TEST_ARGS})
    endif()

    add_custom_target(
      ${fq_target_name}
      DEPENDS ${fq_target_name}.__cmd__
    )

    add_custom_command(
      OUTPUT ${fq_target_name}.__cmd__
      COMMAND ${test_cmd}
      COMMAND_EXPAND_LISTS
      COMMENT "Running hermetic test ${fq_target_name}"
      ${LIBC_HERMETIC_TEST_JOB_POOL}
    )

    set_source_files_properties(${fq_target_name}.__cmd__
      PROPERTIES
        SYMBOLIC "TRUE"
    )
  endif()

  add_dependencies(${HERMETIC_TEST_SUITE} ${fq_target_name})
  if(NOT ${HERMETIC_TEST_IS_GPU_BENCHMARK})
    # If it is a benchmark, it will already have been added to the
    # gpu-benchmark target
    add_dependencies(libc-hermetic-tests ${fq_target_name})
    if(LIBC_HERMETIC_TEST_SUITE)
      add_dependencies(${LIBC_HERMETIC_TEST_SUITE} ${fq_target_name})
      # If a target named ${LIBC_HERMETIC_TEST_SUITE}-build exists, we add the 
      # test executable to it as a dependency. This allows building the 
      # test binaries for Lit without triggering their execution.
      if(TARGET ${LIBC_HERMETIC_TEST_SUITE}-build)
        add_dependencies(${LIBC_HERMETIC_TEST_SUITE}-build ${fq_build_target_name})
      endif()
    endif()
    if(TARGET libc-hermetic-tests-build)
      add_dependencies(libc-hermetic-tests-build ${fq_build_target_name})
    endif()
  endif()
endfunction(add_libc_hermetic)

# A convenience function to add both a unit test as well as a hermetic test.
function(add_libc_test test_name)
  cmake_parse_arguments(
    "LIBC_TEST"
    "UNIT_TEST_ONLY;HERMETIC_TEST_ONLY" # Optional arguments
    "" # Single value arguments
    "" # Multi-value arguments
    ${ARGN}
  )
  if(LIBC_ENABLE_UNITTESTS AND NOT LIBC_TEST_HERMETIC_TEST_ONLY)
    add_libc_unittest(${test_name}.__unit__ ${LIBC_TEST_UNPARSED_ARGUMENTS})
  endif()
  if(LIBC_ENABLE_HERMETIC_TESTS AND NOT LIBC_TEST_UNIT_TEST_ONLY)
    add_libc_hermetic(
      ${test_name}.__hermetic__
      LINK_LIBRARIES
        LibcTest.hermetic
      ${LIBC_TEST_UNPARSED_ARGUMENTS}
    )
    get_fq_target_name(${test_name} fq_test_name)
    if(TARGET ${fq_test_name}.__hermetic__ AND TARGET ${fq_test_name}.__unit__)
      # Tests like the file tests perform file operations on disk file. If we
      # don't chain up the unit test and hermetic test, then those tests will
      # step on each other's files.
      if(NOT LIBC_TEST_HERMETIC_ONLY)
        add_dependencies(${fq_test_name}.__hermetic__ ${fq_test_name}.__unit__)
      endif()
    endif()
  endif()
endfunction(add_libc_test)

# Tests all implementations that can run on the target CPU.
function(add_libc_multi_impl_test name suite)
  get_property(fq_implementations GLOBAL PROPERTY ${name}_implementations)
  foreach(fq_config_name IN LISTS fq_implementations)
    get_target_property(required_cpu_features ${fq_config_name} REQUIRE_CPU_FEATURES)
    cpu_supports(can_run "${required_cpu_features}")
    if(can_run)
      string(FIND ${fq_config_name} "." last_dot_loc REVERSE)
      math(EXPR name_loc "${last_dot_loc} + 1")
      string(SUBSTRING ${fq_config_name} ${name_loc} -1 target_name)
      add_libc_test(
        ${target_name}_test
        SUITE
          ${suite}
        COMPILE_OPTIONS
          ${LIBC_COMPILE_OPTIONS_NATIVE}
        LINK_LIBRARIES
          LibcMemoryHelpers
        ${ARGN}
        DEPENDS
          ${fq_config_name}
          libc.src.__support.macros.sanitizer
      )
      get_fq_target_name(${fq_config_name}_test fq_target_name)
    else()
      message(STATUS "Skipping test for '${fq_config_name}' insufficient host cpu features '${required_cpu_features}'")
    endif()
  endforeach()
endfunction()
