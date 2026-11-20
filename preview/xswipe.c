// xswipe X1 Y1 X2 Y2 [steps=12] [step_ms=15]: press at (X1,Y1), drag in steps, release at (X2,Y2).
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: xswipe X1 Y1 X2 Y2 [steps] [step_ms]\n"); return 1; }
    int x1 = atoi(argv[1]), y1 = atoi(argv[2]), x2 = atoi(argv[3]), y2 = atoi(argv[4]);
    int steps = argc > 5 ? atoi(argv[5]) : 12;
    int ms = argc > 6 ? atoi(argv[6]) : 15;
    if (steps < 1) steps = 1;
    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    XTestFakeMotionEvent(d, -1, x1, y1, CurrentTime); XFlush(d); usleep(30000);
    XTestFakeButtonEvent(d, 1, True, CurrentTime); XFlush(d); usleep(30000);
    for (int i = 1; i <= steps; i++) {
        XTestFakeMotionEvent(d, -1, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, CurrentTime);
        XFlush(d); usleep(ms * 1000);
    }
    XTestFakeButtonEvent(d, 1, False, CurrentTime); XFlush(d); usleep(100000);
    XCloseDisplay(d);
    return 0;
}
