#include "AppDelegate.h"

@implementation MyApplicationDelegate : NSObject
- (id)init {
    if (self = [super init]) {
        NSRect frame = NSMakeRect(0, 0, 200, 200);
        window  = [[[NSWindow alloc] initWithContentRect:frame
                                               styleMask:NSWindowStyleMaskTitled
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO] autorelease];
        [window setBackgroundColor:[NSColor blueColor]];
    }
    return self;
}

- (void)applicationWillFinishLaunching:(NSNotification *)notification {
    [window makeKeyAndOrderFront:self];
}

- (void)dealloc {
    [window release];
    [super dealloc];
}

@end

