#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
int main(void) {
    Display *display = XOpenDisplay(NULL);
    if (!display) return 1;
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 80, 80, 420, 420, 1, 0, 0x66aaff);
    XStoreName(display, window, "Arara Desktop Fixture");
    XClassHint hint = {"AraraDesktopFixture", "AraraDesktopFixture"};
    XSetClassHint(display, window, &hint);
    unsigned long pid = (unsigned long)getpid();
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
    XSelectInput(display, window, ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask | ExposureMask);
    XMapRaised(display, window);
    XFlush(display);
    for (;;) {
        XEvent event;
        XNextEvent(display, &event);
        const char *kind = event.type == ButtonPress ? "mouse_down" : event.type == ButtonRelease ? "mouse_up" : event.type == KeyPress ? "key_down" : event.type == KeyRelease ? "key_up" : NULL;
        if (kind) { struct timespec now; clock_gettime(CLOCK_REALTIME, &now); printf("%s:%.6f\n", kind, (double)now.tv_sec + (double)now.tv_nsec / 1000000000); fflush(stdout); }
    }
}
