# REQUIRES: x86

# A DLL exports a record of a thread-local variable, never the variable, and
# an image that reaches the variable by its section-relative offset cannot
# link against it. The undefined symbol names the record the DLL offers, and
# a DLL that did export the variable gets an error that says why its offset
# cannot be used.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/lib.s -o %t.lib.obj
# RUN: lld-link -dll -noentry -out:%t.lib.dll %t.lib.obj -export:'tls_var$tls',DATA -implib:%t.lib.lib
# RUN: lld-link -dll -noentry -out:%t.old.dll %t.lib.obj -export:tls_var,DATA -implib:%t.old.lib

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/use.s -o %t.use.obj
# RUN: not lld-link -import-slots -entry:main -subsystem:console -out:%t.use.exe %t.use.obj %t.lib.lib 2>&1 | FileCheck --check-prefix=UNDEF %s
# UNDEF:      error: undefined symbol: tls_var
# UNDEF-NEXT: >>> referenced by {{.*}}use.obj
# UNDEF-NEXT: >>> tls_var is a thread-local variable of another image, which exports its record tls_var$tls in its place; mark the declaration with default visibility so that the record is used

# RUN: not lld-link -import-slots -entry:main -subsystem:console -out:%t.old.exe %t.use.obj %t.old.lib 2>&1 | FileCheck --check-prefix=SECREL %s
# SECREL: error: {{.*}}use.obj: tls_var is imported, but is referenced with relocation type IMAGE_REL_AMD64_SECREL, which cannot reach another image; a thread-local variable is reached through the record its image exports, which a declaration with default visibility uses

#--- lib.s
.section .tls$,"dw"
.globl tls_var
tls_var:
  .long 1

.section .rdata,"dr"
.globl "tls_var$tls"
"tls_var$tls":
  .quad _tls_index
  .secrel32 tls_var
  .secrel32 tls_var

.data
.globl _tls_index
_tls_index:
  .long 0

#--- use.s
.text
.globl main
main:
  movl _tls_index(%rip), %eax
  movq %gs:88, %rcx
  movq (%rcx,%rax,8), %rax
  movl tls_var@SECREL32(%rax), %eax
  ret

.data
_tls_index:
  .long 0
