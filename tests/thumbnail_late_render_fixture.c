#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t requested_frame;

static void handle_signal(int signal_number)
{
    if (signal_number == SIGTERM || signal_number == SIGINT)
        running = 0;
    else if (signal_number == SIGUSR1)
        requested_frame = 1;
    else if (signal_number == SIGUSR2)
        requested_frame = 2;
}

static unsigned long named_pixel(Display *display, int screen,
        const char *name, unsigned long fallback)
{
    XColor exact;
    XColor color;

    if (!XAllocNamedColor(display, DefaultColormap(display, screen), name,
                &color, &exact))
        return fallback;
    return color.pixel;
}

static void draw_frame(Display *display, Window window, GC graphics,
        int frame, unsigned long initial, unsigned long first,
        unsigned long second)
{
    unsigned long background = initial;
    unsigned long foreground = first;

    if (frame == 1)
        background = first;
    else if (frame >= 2) {
        background = second;
        foreground = initial;
    }
    XSetForeground(display, graphics, background);
    XFillRectangle(display, window, graphics, 0, 0, 320, 396);
    XSetForeground(display, graphics, foreground);
    XFillRectangle(display, window, graphics, 24, 24, 88, 68);
    XDrawLine(display, window, graphics, 0, 0, 319, 395);
    XFlush(display);
}

static void set_utf8_property(Display *display, Window window,
        const char *name, const char *value)
{
    Atom property = XInternAtom(display, name, False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);

    XChangeProperty(display, window, property, utf8, 8, PropModeReplace,
            (const unsigned char *)value, (int)strlen(value));
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    XEvent event;
    Window window;
    Window surface;
    GC graphics;
    int screen;
    int frame = 0;
    unsigned long initial;
    unsigned long first;
    unsigned long second;

    if (!display) {
        fputs("thumbnail fixture: cannot open DISPLAY\n", stderr);
        return 1;
    }
    screen = DefaultScreen(display);
    initial = named_pixel(display, screen, "#102840",
            BlackPixel(display, screen));
    first = named_pixel(display, screen, "#e03048",
            WhitePixel(display, screen));
    second = named_pixel(display, screen, "#20b060",
            BlackPixel(display, screen));
    window = XCreateSimpleWindow(display, RootWindow(display, screen),
            10, 10, 320, 396, 0, initial, initial);
    if (window == None) {
        XCloseDisplay(display);
        return 1;
    }
    XStoreName(display, window, "MSYS Late Render Thumbnail Fixture");
    set_utf8_property(display, window, "_NET_WM_NAME",
            "MSYS Late Render Thumbnail Fixture");
    set_utf8_property(display, window, "_MSYS_WINDOW_ROLE", "application");
    set_utf8_property(display, window, "_MSYS_APP_ID",
            "org.msys.tests.thumbnail-late-render");
    set_utf8_property(display, window, "_MSYS_COMPONENT_ID",
            "org.msys.tests.thumbnail-late-render:fixture");
    XSelectInput(display, window, StructureNotifyMask);
    /* Draw through a child window, matching Tk/Qt widget trees rather than a
     * synthetic client that only paints its root-level toplevel. */
    surface = XCreateSimpleWindow(display, window, 0, 0, 320, 396, 0,
            initial, initial);
    XSelectInput(display, surface, ExposureMask);
    graphics = XCreateGC(display, surface, 0, NULL);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGUSR1, handle_signal);
    signal(SIGUSR2, handle_signal);
    XMapWindow(display, surface);
    XMapWindow(display, window);
    XFlush(display);

    while (running) {
        while (XPending(display)) {
            XNextEvent(display, &event);
            if (event.type == Expose && event.xexpose.count == 0)
                draw_frame(display, surface, graphics, frame, initial, first,
                        second);
        }
        if (requested_frame != 0) {
            frame = requested_frame;
            requested_frame = 0;
            draw_frame(display, surface, graphics, frame, initial, first,
                    second);
        }
        {
            struct timespec delay = {0, 5 * 1000 * 1000};

            nanosleep(&delay, NULL);
        }
    }

    XFreeGC(display, graphics);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
