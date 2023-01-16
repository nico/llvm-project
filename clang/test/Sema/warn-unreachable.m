// RUN: %clang_cc1 -fsyntax-only -fobjc-exceptions -verify -Wunreachable-code %s

void f(void);

void g1(void) {
  @try {
    f();
    @throw @"";
    f();  // expected-warning{{will never be executed}}
  } @catch(id i) {
    f();
  }

  // Completely empty.
  @try {
  } @catch(...) {
  }

  @try {
    f();
    return;
  } @catch(id i = nil) {  // Catch block should not be marked as unreachable.
    // Empty @catch body.
  }
}

void g2(void) {
  @try {
    // Nested @try.
    @try {
      f();
      @throw @"";
      f(); // expected-warning{{will never be executed}}
    } @catch(...) {
    }
    f();
    @throw @"";
    f(); // expected-warning{{will never be executed}}
  } @catch(...) {
    f();
  }
}

void g3(void) {
  @try {
    @try {
      f();
    } @catch (...) {
      @throw @""; // should exit outer try
    }
    @throw @"";
    f(); // expected-warning{{never be executed}}
  } @catch (...) {
  }
}

void g4(void) {
  @try {
    f();
  } @finally {
    return;
  }
  f(); // expected-warning{{never be executed}}
}

void g5() {
  @try {
    // The warning should work with an empty @try body too
    // (which leads to a nullptr CFG block for the try body).
  } @finally {
    return;
  }
  f(); // expected-warning{{never be executed}}
}
