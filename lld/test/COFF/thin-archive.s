# REQUIRES: x86

# RUN: rm -rf %t.dir
# RUN: split-file %s %t.dir

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc \
# RUN:     -o %t.main.obj %t.dir/main.s

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -o %t.lib.obj \
# RUN:     %S/Inputs/mangled-symbol.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc \
# RUN:     -o %t.foo.obj %t.dir/foo.s
# RUN: lld-link /lib /out:%t.lib %t.lib.obj %t.foo.obj
# RUN: lld-link /lib /llvmlibthin /out:%t_thin.lib %t.lib.obj %t.foo.obj

# RUN: lld-link /entry:main %t.main.obj %t.lib /out:%t.exe 2>&1 | \
# RUN:     FileCheck --allow-empty %s
# RUN: lld-link /entry:main %t.main.obj %t_thin.lib /out:%t.exe 2>&1 | \
# RUN:     FileCheck --allow-empty %s
# RUN: lld-link /entry:main %t.main.obj /wholearchive:%t_thin.lib /out:%t.exe 2>&1 | \
# RUN:     FileCheck --allow-empty %s

# RUN: rm %t.lib.obj
# RUN: lld-link /entry:main %t.main.obj %t.lib /out:%t.exe 2>&1 | \
# RUN:     FileCheck --allow-empty %s
# RUN: env LLD_IN_TEST=1 not lld-link /entry:main %t.main.obj %t_thin.lib \
# RUN:     /out:%t.exe 2>&1 | FileCheck --check-prefix=NOOBJ %s
# RUN: env LLD_IN_TEST=1 not lld-link /entry:main %t.main.obj %t_thin.lib /out:%t.exe \
# RUN:     /demangle:no 2>&1 | FileCheck --check-prefix=NOOBJNODEMANGLE %s

# CHECK-NOT: error: could not get the buffer for the member defining
# NOOBJ: error: could not get the buffer for the member defining symbol int __cdecl f(void): {{.*}}.lib({{.*}}.lib.obj):
# NOOBJNODEMANGLE: error: could not get the buffer for the member defining symbol ?f@@YAHXZ: {{.*}}.lib({{.*}}.lib.obj):

#--- main.s
	.text

	.def main
		.scl 2
		.type 32
	.endef
	.global main
main:
	call "?f@@YAHXZ"
	call foo
	retq $0

#--- foo.s
	.text

	.def foo
		.scl 2
		.type 32
	.endef
	.global foo
foo:
	retq $0
