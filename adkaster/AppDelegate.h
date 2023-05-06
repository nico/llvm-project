#include <Cocoa/Cocoa.h>

@interface MyApplicationDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow * window;
}
@end
