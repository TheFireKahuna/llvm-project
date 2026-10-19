# windows_itanium_joint_link(<name> <target>...)
#
# Links the given executable and shared library targets jointly, as the members
# of one joint link, in the order given, which decides which member keeps a
# vague entity that several keep and none imports from another.
#
# Each member gets a twin, <target>.jointscan, built from the member's objects
# with its link options and libraries, that links it once with -lldjointscan.
# The scan writes no image, but the member's summary, under the twin's output
# name, and, where none is there yet, the twin's import library, which the
# twins of the members that link the member link in its place. One custom
# command, <name>.jointmerge, merges the summaries into a fact file for each
# member, which is replaced only when it changes, and each member links with
# -lldjoint naming its own. Call it once the members' libraries are added.
function(windows_itanium_joint_link name)
  if(NOT ARGN)
    message(FATAL_ERROR "windows_itanium_joint_link(${name}) names no members")
  endif()
  set(dir "${CMAKE_CURRENT_BINARY_DIR}/${name}.joint")

  # The merge runs lld-link, next to the compiler where there is one.
  get_filename_component(hint "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(WINDOWS_ITANIUM_LLD_LINK NAMES lld-link HINTS "${hint}")
  if(NOT WINDOWS_ITANIUM_LLD_LINK)
    message(FATAL_ERROR "windows_itanium_joint_link(${name}) finds no lld-link")
  endif()

  # A twin's objects are its member's, which it links rather than lists, so
  # that CMake does not order it after its member; a target needs a source, so
  # it lists an empty header, which builds nothing.
  set(header "${dir}/jointscan.h")
  file(WRITE "${header}" "")

  set(summaries)
  set(facts)
  foreach(member IN LISTS ARGN)
    get_target_property(type ${member} TYPE)
    if(type STREQUAL "EXECUTABLE")
      add_executable(${member}.jointscan EXCLUDE_FROM_ALL "${header}")
    elseif(type STREQUAL "SHARED_LIBRARY")
      add_library(${member}.jointscan SHARED EXCLUDE_FROM_ALL "${header}")
    else()
      message(FATAL_ERROR "windows_itanium_joint_link(${name}): ${member} is "
                          "not an executable or a shared library")
    endif()

    # The twin's image is the member's by name, and holds its summary.
    get_target_property(prefix ${member} PREFIX)
    get_target_property(base ${member} OUTPUT_NAME)
    get_target_property(suffix ${member} SUFFIX)
    if(NOT base)
      set(base ${member})
    endif()
    if(type STREQUAL "EXECUTABLE")
      set(defaultPrefix "")
      set(defaultSuffix "${CMAKE_EXECUTABLE_SUFFIX}")
    else()
      set(defaultPrefix "${CMAKE_SHARED_LIBRARY_PREFIX}")
      set(defaultSuffix "${CMAKE_SHARED_LIBRARY_SUFFIX}")
    endif()
    if(prefix STREQUAL "prefix-NOTFOUND")
      set(prefix "${defaultPrefix}")
    endif()
    if(suffix STREQUAL "suffix-NOTFOUND")
      set(suffix "${defaultSuffix}")
    endif()
    set(scanDir "${dir}/${member}")
    set(summary "${scanDir}/${prefix}${base}${suffix}")
    set(fact "${dir}/${prefix}${base}${suffix}.jfacts")
    set_target_properties(${member}.jointscan PROPERTIES
      PREFIX "${prefix}" OUTPUT_NAME "${base}" SUFFIX "${suffix}"
      RUNTIME_OUTPUT_DIRECTORY "${scanDir}"
      LIBRARY_OUTPUT_DIRECTORY "${scanDir}"
      ARCHIVE_OUTPUT_DIRECTORY "${scanDir}"
      PDB_OUTPUT_DIRECTORY "${scanDir}")
    # A header gives the twin no language of its own.
    get_target_property(language ${member} LINKER_LANGUAGE)
    if(NOT language)
      if(CMAKE_CXX_COMPILER_LOADED)
        set(language CXX)
      else()
        set(language C)
      endif()
    endif()
    set_target_properties(${member}.jointscan PROPERTIES
                          LINKER_LANGUAGE ${language})
    foreach(property LINK_OPTIONS LINK_FLAGS WIN32_EXECUTABLE LINK_DEPENDS)
      get_target_property(value ${member} ${property})
      if(value)
        set_property(TARGET ${member}.jointscan PROPERTY ${property} "${value}")
      endif()
    endforeach()
    target_link_options(${member}.jointscan PRIVATE
                        "LINKER:-lldjointscan:${summary}")
    set(objects "${scanDir}.objects")
    file(GENERATE OUTPUT "${objects}"
         CONTENT "\"$<JOIN:$<TARGET_OBJECTS:${member}>,\"\n\">\"\n")
    target_link_options(${member}.jointscan PRIVATE "LINKER:@${objects}")
    set_property(TARGET ${member}.jointscan APPEND PROPERTY LINK_DEPENDS
                 "${objects}" $<TARGET_OBJECTS:${member}>)

    # Another member is linked through its twin's import library, which the
    # twin's scan writes.
    get_target_property(libraries ${member} LINK_LIBRARIES)
    if(libraries)
      foreach(library IN LISTS libraries)
        if(library IN_LIST ARGN)
          set(library ${library}.jointscan)
        endif()
        target_link_libraries(${member}.jointscan PRIVATE ${library})
      endforeach()
    endif()

    target_link_options(${member} PRIVATE "LINKER:-lldjoint:${fact}")
    set_property(TARGET ${member} APPEND PROPERTY LINK_DEPENDS "${fact}")
    list(APPEND summaries "${summary}")
    list(APPEND facts "${fact}")
  endforeach()

  set(scans ${ARGN})
  list(TRANSFORM scans APPEND .jointscan)
  add_custom_command(OUTPUT ${facts}
    COMMAND "${WINDOWS_ITANIUM_LLD_LINK}" -lldjointmerge "-out:${dir}"
            ${summaries}
    DEPENDS ${scans} ${summaries}
    COMMENT "Merging the joint link ${name}"
    VERBATIM)
  # Only the members' links wait for the facts, through LINK_DEPENDS, so their
  # objects are what the twins link.
  add_custom_target(${name}.jointmerge DEPENDS ${facts})
endfunction()
