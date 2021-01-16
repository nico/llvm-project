# REQUIRES: x86

# RUN: rm -rf %t
# RUN: split-file %s %t

# Tests that linking a .lib file that contains .obj files with the wrong arch causes an error.

# RUN: llvm-mc -filetype=obj -triple=i386-windows-msvc -o %t/f-32.obj %t/f.s
# RUN: lld-link -lib -out:%t/f-32.lib %t/f-32.obj
# RUN: llvm-mc -filetype=obj -triple=i386-windows-msvc -o %t/main-32.obj %t/main.s
#
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -o %t/f-64.obj %t/f.s
# RUN: lld-link -lib -out:%t/f-64.lib %t/f-64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -o %t/main-64.obj %t/main.s


# Basics: Make a .lib file with 32-bit .obj files, use it in a 64-bit link, it should error.
# RUN: not lld-link %t/main-64.obj %t/f-32.lib 2>&1 | FileCheck --check-prefix=CHECK64 -DLIB=%t/f-32.lib %s
# CHECK64: lld-link: error: [[LIB]]: machine type x86 conflicts with x64
# CHECK64: lld-link: error: {{.*}}f-32.lib(f-32.obj): machine type x86 conflicts with x64
# XXX omit 2nd line

# Same for the other way round.
# RUN: not lld-link %t/main-32.obj %t/f-64.lib 2>&1 | FileCheck --check-prefix=CHECK32 -DLIB=%t/f-64.lib %s
# CHECK32: lld-link: error: [[LIB]]: machine type x64 conflicts with x86
# CHECK32: lld-link: error: {{.*}}f-64.lib(f-64.obj): machine type x64 conflicts with x86
# XXX omit 2nd line

# /wholeArchive uses a different codepath, which errors on the .obj file instead.
# RUN: not lld-link %t/main-32.obj /wholeArchive %t/f-64.lib 2>&1 | FileCheck --check-prefix=CHECKWHOLE32 -DLIB=%t/f-64.lib %s
# CHECKWHOLE32-NOT: lld-link: error: [[LIB]]: machine type x64 conflicts with x86
# CHECKWHOLE32: lld-link: error: {{.*}}f-64.lib(f-64.obj): machine type x64 conflicts with x86

# Test bitcode archives.
# RUN: llvm-as -o %t/f-bitcode-64.obj %t/f.ll
# RUN: lld-link -lib -out:%t/f-bitcode-64.lib %t/f-bitcode-64.obj

# RUN: not lld-link %t/main-32.obj %t/f-bitcode-64.lib 2>&1 | FileCheck --check-prefix=CHECKBC32 -DLIB=%t/f-bitcode-64.lib %s
# CHECKBC32: lld-link: error: [[LIB]]: machine type x64 conflicts with x86
# CHECKBC32: lld-link: error: {{.*}}f-bitcode-64.lib(f-bitcode-64.obj): machine type x64 conflicts with x86
# XXX omit 2nd line

# RUN: not lld-link %t/main-32.obj /wholeArchive %t/f-bitcode-64.lib 2>&1 | FileCheck --check-prefix=CHECKWHOLEBC32 -DLIB=%t/f-bitcode-64.lib %s
# CHECKWHOLEBC32-NOT: lld-link: error: [[LIB]]: machine type x64 conflicts with x86
# CHECKWHOLEBC32: lld-link: error: {{.*}}f-bitcode-64.lib(f-bitcode-64.obj): machine type x64 conflicts with x86

#--- f.s
	.text
	.def f
	.scl 2
	.type 32
	.endef
	.global f
f:
	ret $0

	.section .drectve,"rd"
	.ascii " /EXPORT:f"

#--- f.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"
define i32 @f() { ret i32 0 }

#--- main.s
	.def	 main;
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",one_only,main
	.globl	main
main:
	call	f
	ret
