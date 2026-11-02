// RUN: %clang -### -c -flazy-function-instantiation %s 2>&1 | FileCheck %s
// RUN: %clang -### -c -flazy-function-instantiation \
// RUN:   -fno-lazy-function-instantiation %s 2>&1 | FileCheck --check-prefix=NO %s
// RUN: %clang -### -c %s 2>&1 | FileCheck --check-prefix=NO %s
// CHECK: "-cc1"{{.*}} "-flazy-function-instantiation"
// NO-NOT: "-flazy-function-instantiation"
