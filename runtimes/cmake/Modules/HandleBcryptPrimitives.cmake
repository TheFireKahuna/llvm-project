# HandleBcryptPrimitives.cmake
#
# Generate import library for bcryptprimitives.dll.
# The Windows SDK ships bcrypt.lib but not bcryptprimitives.lib.
# ProcessPrng (used for security cookie init) is exported from
# bcryptprimitives.dll, so we generate a minimal import library.

include(GenerateImportLibrary)

function(generate_bcryptprimitives_import_library output_var)
  generate_import_library(${output_var} bcryptprimitives
    "LIBRARY bcryptprimitives.dll\nEXPORTS\n    ProcessPrng\n")
  set(${output_var} "${${output_var}}" PARENT_SCOPE)
endfunction()
