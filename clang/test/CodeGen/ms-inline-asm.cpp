// REQUIRES: x86-registered-target
// RUN: %clang_cc1 -x c++ %s -triple i386-apple-darwin10 -fasm-blocks -emit-llvm -o - -std=c++11 | FileCheck %s

// rdar://13645930

struct Foo {
  static int *ptr;
  static int a, b;
  int arr[4];
  struct Bar {
    static int *ptr;
    char arr[2];
  };
};

int gvar = 10;
void t2() {
  int lvar = 10;
  __asm mov eax, offset Foo::ptr
  __asm mov eax, offset Foo::Bar::ptr
// CHECK-LABEL: define void @_Z2t2v()
// CHECK: call void asm sideeffect inteldialect
// CHECK-SAME: mov eax, $0
// CHECK-SAME: mov eax, $1
// CHECK-SAME: "r,r,~{eax},~{dirflag},~{fpsr},~{flags}"(i32** @_ZN3Foo3ptrE, i32** @_ZN3Foo3Bar3ptrE)
}
