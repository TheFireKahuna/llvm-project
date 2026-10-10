# REQUIRES: x86

## A C++ function that only an archive defines is found by its unmangled
## name, whether the archive's symbol table is sorted, as llvm-lib writes it,
## or not: the subsystem and entry point are inferred from such a wmain, and
## an export names it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc empty.s -o empty.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc zzz.s -o zzz.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wmain.s -o wmain.obj
# RUN: llvm-lib -out:sorted.lib zzz.obj crt.obj wmain.obj
# RUN: llvm-ar --format=gnu rcs unsorted.lib zzz.obj crt.obj wmain.obj
# RUN: llvm-nm --print-armap unsorted.lib | FileCheck --check-prefix=ARMAP %s

# ARMAP:      Archive map
# ARMAP-NEXT: zzz in zzz.obj
# ARMAP-NEXT: wmainCRTStartup in crt.obj
# ARMAP-NEXT: ?wmain@@YAHXZ in wmain.obj

# RUN: lld-link -out:sorted.exe empty.obj sorted.lib -verbose 2>&1 \
# RUN:   | FileCheck --check-prefix=ENTRY %s
# RUN: llvm-readobj --file-headers sorted.exe | FileCheck --check-prefix=CUI %s
# RUN: lld-link -out:unsorted.exe empty.obj unsorted.lib -verbose 2>&1 \
# RUN:   | FileCheck --check-prefix=ENTRY %s
# RUN: llvm-readobj --file-headers unsorted.exe \
# RUN:   | FileCheck --check-prefix=CUI %s

# ENTRY: Entry name inferred: wmainCRTStartup
# CUI: Subsystem: IMAGE_SUBSYSTEM_WINDOWS_CUI

# RUN: lld-link -dll -noentry -out:sorted.dll empty.obj sorted.lib \
# RUN:   -export:wmain -verbose 2>&1 | FileCheck --check-prefix=ALIAS %s
# RUN: llvm-readobj --coff-exports sorted.dll | FileCheck --check-prefix=EXPORT %s
# RUN: lld-link -dll -noentry -out:unsorted.dll empty.obj unsorted.lib \
# RUN:   -export:wmain -verbose 2>&1 | FileCheck --check-prefix=ALIAS %s
# RUN: llvm-readobj --coff-exports unsorted.dll \
# RUN:   | FileCheck --check-prefix=EXPORT %s

# ALIAS: wmain aliased to ?wmain@@YAHXZ
# EXPORT: Name: wmain

#--- empty.s

#--- zzz.s
.globl zzz
zzz:
  ret

#--- crt.s
.globl wmainCRTStartup
wmainCRTStartup:
  call "?wmain@@YAHXZ"
  ret

#--- wmain.s
.globl "?wmain@@YAHXZ"
"?wmain@@YAHXZ":
  ret
