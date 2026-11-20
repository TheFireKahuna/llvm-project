# REQUIRES: x86

## --exclude-libs keeps an archive member's definitions out of the exports (its
## directives and export-all alike); archives by name with or without extension,
## or ALL; objects given alone and /export exports are never covered.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu obj.s -o obj.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu b.s -o b.obj
# RUN: rm -f a.lib libb.a && llvm-ar rcs a.lib a.obj && llvm-ar rcs libb.a b.obj

# DEFINE: %{link} = lld-link -dll -noentry obj.obj a.lib libb.a -out:out.dll
# DEFINE: %{check} = llvm-readobj --coff-exports out.dll | \
# DEFINE:   FileCheck --implicit-check-not=Name: %s

# RUN: %{link} && %{check} --check-prefixes=OBJ,A,B
# RUN: %{link} -exclude-libs:a.lib && %{check} --check-prefixes=OBJ,B
# RUN: %{link} --exclude-libs=a,libb.a,none && %{check} --check-prefix=OBJ
# RUN: %{link} --exclude-libs ALL -export:a2 && %{check} --check-prefixes=A2,OBJ
# RUN: lld-link -lldmingw -dll -noentry obj.obj a.lib libb.a -out:out.dll \
# RUN:   -export-all-symbols -exclude-libs:libb:other
# RUN: %{check} --check-prefixes=A,A2,OBJ

# A:       Name: a{{$}}
# A2:      Name: a2
# B:       Name: b
# OBJ:     Name: obj

#--- obj.s
.globl obj
obj:
  call a
  call b
  ret
.section .drectve
.ascii " -export:obj"

#--- a.s
.globl a, a2
a:
a2:
  ret
.section .drectve
.ascii " -export:a"

#--- b.s
.globl b
b:
  ret
.section .drectve
.ascii " -export:b"
