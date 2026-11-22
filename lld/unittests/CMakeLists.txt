function(add_lld_unittests test_dirname)
  add_unittest(LLDUnitTests ${test_dirname} ${ARGN})
endfunction()

add_subdirectory(AsLibAll)
add_subdirectory(AsLibELF)
if("X86" IN_LIST LLVM_TARGETS_TO_BUILD)
  add_subdirectory(COFF)
endif()
