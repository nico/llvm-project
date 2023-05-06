#import "AppDelegate.h"

int main(int argc, char * argv[]) {
    NSAutoreleasePool * pool = [[NSAutoreleasePool alloc] init];
    NSApplication * application = [NSApplication sharedApplication];

    MyApplicationDelegate * appDelegate = [[[MyApplicationDelegate alloc] init] autorelease];

    [application setDelegate:appDelegate];
    [application run];

    [pool drain];

    return EXIT_SUCCESS;
}
