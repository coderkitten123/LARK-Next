// Tiny XTEST tap injector for remote UI testing on the Kindle.
// Usage: xtap X Y [count]
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: xtap X Y [count]\n"); return 1; }
    int x = atoi(argv[1]), y = atoi(argv[2]);
    int count = argc > 3 ? atoi(argv[3]) : 1;
    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    for (int i = 0; i < count; i++) {
        XTestFakeMotionEvent(d, -1, x, y, CurrentTime);
        XFlush(d);
        usleep(20000);
        XTestFakeButtonEvent(d, 1, True, CurrentTime);
        XFlush(d);
        usleep(60000);
        XTestFakeButtonEvent(d, 1, False, CurrentTime);
        XFlush(d);
        usleep(300000);
    }
    XCloseDisplay(d);
    return 0;
}
