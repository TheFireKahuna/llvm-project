# REQUIRES: x86

## The symbols of an archive read once the machine is known are entered only
## when looked up, as the first archive that defines each name would have
## entered them: a name two archives define, referenced from a member loaded
## after both are read, comes from the first; a C++ wmain only an archive
## defines is found by its unmangled name; and /includeglob matches archive
## symbols.

# RUN: rm -rf %t && mkdir %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=x86_64-windows-msvc
# RUN: echo '.globl wmainCRTStartup; wmainCRTStartup: call g; ret' | %{mc} -o main.obj
# RUN: echo '.globl h; h: ret' | %{mc} -o h1.obj
# RUN: echo '.globl g; g: call h; ret' | %{mc} -o g2.obj
# RUN: echo '.globl h; h: ret' | %{mc} -o h2.obj
# RUN: echo '.globl "?wmain@@YAHXZ"; "?wmain@@YAHXZ": ret' | %{mc} -o wmain.obj
# RUN: echo '.globl glob_x; glob_x: ret' | %{mc} -o glob.obj
# RUN: llvm-lib -out:one.lib h1.obj
# RUN: llvm-lib -out:two.lib g2.obj h2.obj wmain.obj glob.obj

# RUN: lld-link -out:main.exe main.obj one.lib two.lib -includeglob:glob_* \
# RUN:   -verbose 2>&1 | FileCheck %s --implicit-check-not=h2.obj \
# RUN:   --implicit-check-not=wmain.obj
# RUN: llvm-readobj --file-headers main.exe | FileCheck --check-prefix=CUI %s

# CHECK: Loaded two.lib(g2.obj) for g
# CHECK: Loaded one.lib(h1.obj) for h
# CHECK: Entry name inferred: wmainCRTStartup
# CHECK: Loaded two.lib(glob.obj) for glob_x
# CUI: Subsystem: IMAGE_SUBSYSTEM_WINDOWS_CUI
