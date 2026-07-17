#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fixture_window {
    const char *name;
    const char *app_id;
    const char *component_id;
    const char *role;
    int x;
    int y;
    int width;
    int height;
};

static void set_text_property(Display *display, Window window,
        const char *name, const char *value)
{
    Atom property = XInternAtom(display, name, False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);

    XChangeProperty(display, window, property, utf8, 8, PropModeReplace,
            (const unsigned char *)value, (int)strlen(value));
}

static Window create_fixture_window(Display *display, Window root,
        const struct fixture_window *fixture)
{
    XSetWindowAttributes attributes;
    XClassHint class_hint;
    Window window;

    memset(&attributes, 0, sizeof(attributes));
    attributes.override_redirect = True;
    attributes.background_pixel = WhitePixel(display, DefaultScreen(display));
    window = XCreateWindow(display, root, fixture->x, fixture->y,
            (unsigned int)fixture->width, (unsigned int)fixture->height,
            0, CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWBackPixel, &attributes);
    XStoreName(display, window, fixture->name);
    set_text_property(display, window, "_NET_WM_NAME", fixture->name);
    if (fixture->app_id)
        set_text_property(display, window, "_MSYS_APP_ID", fixture->app_id);
    if (fixture->component_id)
        set_text_property(display, window, "_MSYS_COMPONENT_ID",
                fixture->component_id);
    if (fixture->role)
        set_text_property(display, window, "_MSYS_WINDOW_ROLE", fixture->role);
    class_hint.res_name = (char *)"msys-or-fixture";
    class_hint.res_class = (char *)"MsysOrFixture";
    XSetClassHint(display, window, &class_hint);
    XSelectInput(display, window, ButtonPressMask | ButtonReleaseMask |
            PointerMotionMask | StructureNotifyMask);
    XMapWindow(display, window);
    return window;
}

int main(void)
{
    static const struct fixture_window fixtures[] = {
        {"MSYS OR Navigation", "org.msys.tests.navigation",
         "org.msys.tests:system-ui", "navigation-bar", 0, 438, 320, 42},
        {"MSYS OR Chrome", "org.msys.tests.chrome",
         "org.msys.tests:system-ui", "system-chrome", 0, 0, 320, 42},
        {"MSYS OR Tasks", "org.msys.tests.tasks",
         "org.msys.tests:system-ui", "task-switcher", 0, 42, 320, 396},
        {"MSYS OR Notifications", "org.msys.tests.notification-center",
         "org.msys.tests:system-ui", "notification-center", 0, 42, 320, 396},
        {"MSYS OR Controls", "org.msys.tests.controls",
         "org.msys.tests:system-ui", "quick-controls", 0, 42, 320, 396},
        {"MSYS OR Toast", "org.msys.tests.toast",
         "org.msys.tests:system-ui", "notification-presenter", 10, 52, 300, 76},
        {"MSYS OR Transition", "org.msys.tests.transition",
         "org.msys.tests:system-ui", "transition-presenter", 0, 42, 320, 396},
        /* These must remain invisible to policy/debug routing. */
        {"Untrusted OR Popup", "org.vendor.popup", NULL,
         "navigation-bar", 40, 100, 120, 80},
        {"Untrusted OR Tooltip", "org.vendor.tooltip",
         "org.vendor:tooltip", NULL, 60, 140, 120, 40}
    };
    Display *display = XOpenDisplay(NULL);
    Window root;
    size_t index;
    unsigned int pressed = 0;
    unsigned int released = 0;
    unsigned int moved = 0;

    if (!display) {
        fputs("override redirect fixture: cannot open DISPLAY\n", stderr);
        return 1;
    }
    root = DefaultRootWindow(display);
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++)
        (void)create_fixture_window(display, root, &fixtures[index]);
    XFlush(display);
    puts("ready");
    fflush(stdout);

    while (pressed < 2 || released < 2 || moved == 0) {
        XEvent event;

        XNextEvent(display, &event);
        if (event.type == ButtonPress)
            pressed++;
        else if (event.type == ButtonRelease)
            released++;
        else if (event.type == MotionNotify)
            moved++;
    }
    printf("events press=%u release=%u motion=%u\n", pressed, released, moved);
    fflush(stdout);
    XCloseDisplay(display);
    return 0;
}
