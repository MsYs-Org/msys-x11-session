#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "msys_layout.h"
#include "msys_x11_agent.h"
#include "msys_x11_policy_api.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static int wm_conflict;

#define MAX_PROCESS_ENVIRON_BYTES (64U * 1024U)
#define MAX_IDENTITY_BYTES 256U
#define MAX_DEBUG_TITLE_BYTES 1024U
#define MAX_WINDOW_PID (1UL << 30)
#define LAYOUT_CONFIG_PROPERTY "_MSYS_LAYOUT_CONFIG_V1"
#define LAYOUT_EFFECTIVE_PROPERTY "_MSYS_LAYOUT_EFFECTIVE_V1"
#define DISPLAY_LAYOUT_PROPERTY "_MSYS_DISPLAY_SESSION_LAYOUT_V1"
#define DISPLAY_LAYOUT_SCHEMA "msys.display-session.layout.v1"
#define DISPLAY_MATRIX_TEXT_MAX 192
#define WINDOW_ID_PROPERTY "_MSYS_WINDOW_ID_V1"
#define WINDOW_ID_SCHEMA "msys.x11-window.v1"
#define WINDOW_ID_MAX 192
#define DEBUG_GESTURE_MAX_MS 10000
#define DEBUG_GESTURE_FRAME_MS 16
#define THUMBNAIL_DIRECTORY "window-thumbnails"
#define THUMBNAIL_MAX_WIDTH 288
#define THUMBNAIL_MAX_HEIGHT 360
#define THUMBNAIL_MAX_FILE_BYTES (2U * 1024U * 1024U)

static char window_session[80];

static int set_text_property(Display *display, Window window, Atom property,
        const char *text);
static int request_window_close(Display *display, Window target);
static long window_wm_state(Display *display, Window window);

struct display_layout_signal {
    int width;
    int height;
    int depth;
    int input_enabled;
    char input_matrix[DISPLAY_MATRIX_TEXT_MAX];
};

enum debug_gesture_selector {
    DEBUG_GESTURE_NONE = 0,
    DEBUG_GESTURE_IDENTITY,
    DEBUG_GESTURE_TITLE,
    DEBUG_GESTURE_WINDOW
};

struct debug_xtest_api {
    void *library;
    Bool (*query_extension)(Display *, int *, int *, int *, int *);
    int (*fake_motion)(Display *, int, int, int, unsigned long);
    int (*fake_button)(Display *, unsigned int, Bool, unsigned long);
};

enum window_kind {
    WINDOW_APPLICATION = 0,
    WINDOW_LAUNCHER,
    WINDOW_INPUT_METHOD,
    WINDOW_RECENTS,
    WINDOW_CHOOSER,
    WINDOW_NOTIFICATION,
    WINDOW_CHROME,
    WINDOW_NAVIGATION,
    WINDOW_TRANSITION,
    WINDOW_SHIELD
};

enum window_layer {
    LAYER_APPLICATION = 0,
    LAYER_INPUT_METHOD = 5,
    LAYER_RECENTS = 10,
    LAYER_CHOOSER = 15,
    LAYER_NOTIFICATION = 20,
    LAYER_CHROME = 30,
    LAYER_NAVIGATION = 31,
    LAYER_TRANSITION = 35,
    LAYER_SHIELD = 40
};

struct window_metadata {
    char *title;
    char *wm_instance;
    char *wm_class;
    char *app_id;
    char *component_id;
    char *process_app_id;
    char *process_component_id;
    char *declared_identity;
    char *role;
};

struct identity_rule {
    const char *identity;
    enum window_kind kind;
};

struct configured_role {
    const char *environment;
    enum window_kind kind;
};

/*
 * These are stable identities from the reference shell manifest.  A replacement
 * provider can advertise its role with _MSYS_WINDOW_ROLE or add its identity to
 * one of the MSYS_X11_*_IDENTITIES comma-separated environment variables below.
 */
static const struct identity_rule default_identity_rules[] = {
    {"org.msys.shell.launcher", WINDOW_LAUNCHER},
    {"org.msys.input.touch", WINDOW_INPUT_METHOD},
    {"org.msys.shell.task-switcher", WINDOW_RECENTS},
    {"org.msys.shell.notification-center", WINDOW_RECENTS},
    {"org.msys.shell.intent-chooser", WINDOW_CHOOSER},
    {"org.msys.shell.notifications", WINDOW_NOTIFICATION},
    {"org.msys.shell.system-chrome", WINDOW_CHROME},
    {"org.msys.shell.navigation", WINDOW_NAVIGATION},
    {"org.msys.shell.navigation-pill", WINDOW_NAVIGATION},
    {"org.msys.shell.transitions", WINDOW_TRANSITION},
    {"org.msys.shell.screen-shield", WINDOW_SHIELD}
};

static const struct configured_role configured_roles[] = {
    {"MSYS_X11_LAUNCHER_IDENTITIES", WINDOW_LAUNCHER},
    {"MSYS_X11_INPUT_METHOD_IDENTITIES", WINDOW_INPUT_METHOD},
    {"MSYS_X11_RECENTS_IDENTITIES", WINDOW_RECENTS},
    {"MSYS_X11_CHOOSER_IDENTITIES", WINDOW_CHOOSER},
    {"MSYS_X11_NOTIFICATION_IDENTITIES", WINDOW_NOTIFICATION},
    {"MSYS_X11_CHROME_IDENTITIES", WINDOW_CHROME},
    {"MSYS_X11_NAVIGATION_IDENTITIES", WINDOW_NAVIGATION},
    {"MSYS_X11_TRANSITION_IDENTITIES", WINDOW_TRANSITION},
    {"MSYS_X11_SHIELD_IDENTITIES", WINDOW_SHIELD}
};

static void handle_signal(int signo)
{
    (void)signo;
    running = 0;
}

static int handle_xerror(Display *display, XErrorEvent *event)
{
    (void)display;
    if (event->error_code == BadAccess)
        wm_conflict = 1;
    return 0;
}

static int starts_with(const char *value, const char *prefix)
{
    return value && prefix && strncmp(value, prefix, strlen(prefix)) == 0;
}

static int text_equal(const char *left, const char *right)
{
    return left && right && strcasecmp(left, right) == 0;
}

static int text_equal_n(const char *left, const char *right, size_t right_length)
{
    return left && right && strlen(left) == right_length &&
        strncasecmp(left, right, right_length) == 0;
}

static char *window_property_string(Display *display, Window window,
        const char *property_name)
{
    Atom property;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *value = NULL;
    char *result = NULL;

    property = XInternAtom(display, property_name, True);
    if (property == None)
        return NULL;
    if (XGetWindowProperty(display, window, property, 0, 1024, False,
                AnyPropertyType, &actual_type, &actual_format, &item_count,
                &bytes_after, &value) != Success)
        return NULL;
    (void)actual_type;
    (void)bytes_after;
    if (value && actual_format == 8 && item_count > 0) {
        result = malloc(item_count + 1);
        if (result) {
            memcpy(result, value, item_count);
            result[item_count] = '\0';
        }
    }
    if (value)
        XFree(value);
    return result;
}

static int window_process_id(Display *display, Window window, pid_t *pid)
{
    Atom property;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *value = NULL;
    unsigned long raw_pid;
    int valid = 0;

    property = XInternAtom(display, "_NET_WM_PID", True);
    if (property == None)
        return 0;
    if (XGetWindowProperty(display, window, property, 0, 1, False,
                XA_CARDINAL, &actual_type, &actual_format, &item_count,
                &bytes_after, &value) != Success)
        return 0;
    (void)bytes_after;
    if (value && actual_type == XA_CARDINAL && actual_format == 32 &&
            item_count == 1) {
        /* Xlib represents a format-32 property as an array of unsigned long,
         * including on LP64 hosts. */
        raw_pid = *(unsigned long *)value;
        if (raw_pid > 0 && raw_pid <= MAX_WINDOW_PID) {
            *pid = (pid_t)raw_pid;
            valid = 1;
        }
    }
    if (value)
        XFree(value);
    return valid;
}

static char *environment_value(const unsigned char *environment, size_t length,
        const char *name)
{
    size_t offset = 0;
    size_t name_length = strlen(name);

    while (offset < length) {
        size_t end = offset;
        size_t value_length;
        char *value;

        while (end < length && environment[end] != '\0')
            end++;
        /* A capped/truncated final entry is never trusted. */
        if (end >= length)
            return NULL;
        if (end > offset + name_length + 1 &&
                memcmp(environment + offset, name, name_length) == 0 &&
                environment[offset + name_length] == '=') {
            value_length = end - offset - name_length - 1;
            if (value_length > MAX_IDENTITY_BYTES)
                return NULL;
            value = malloc(value_length + 1);
            if (!value)
                return NULL;
            memcpy(value, environment + offset + name_length + 1,
                    value_length);
            value[value_length] = '\0';
            return value;
        }
        offset = end + 1;
    }
    return NULL;
}

static void apply_process_environment(struct window_metadata *metadata,
        const unsigned char *environment, size_t length)
{
    if (!metadata->process_app_id)
        metadata->process_app_id = environment_value(environment, length,
                "MSYS_APP_ID");
    if (!metadata->process_component_id)
        metadata->process_component_id = environment_value(environment, length,
                "MSYS_COMPONENT_ID");
    if (!metadata->declared_identity)
        metadata->declared_identity = environment_value(environment, length,
                "MSYS_WINDOW_IDENTITY");
    if (!metadata->role)
        metadata->role = environment_value(environment, length,
                "MSYS_WINDOW_ROLE");
}

static void load_process_environment(Display *display, Window window,
        struct window_metadata *metadata)
{
    unsigned char *environment;
    char path[64];
    size_t used = 0;
    pid_t pid;
    int fd;
    int path_length;

    if (!window_process_id(display, window, &pid))
        return;
    path_length = snprintf(path, sizeof(path), "/proc/%ld/environ", (long)pid);
    if (path_length < 0 || (size_t)path_length >= sizeof(path))
        return;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return;
    environment = malloc(MAX_PROCESS_ENVIRON_BYTES);
    if (!environment) {
        close(fd);
        return;
    }
    while (used < MAX_PROCESS_ENVIRON_BYTES) {
        ssize_t count = read(fd, environment + used,
                MAX_PROCESS_ENVIRON_BYTES - used);

        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fd);
    if (used > 0)
        apply_process_environment(metadata, environment, used);
    free(environment);
}

static char *window_name(Display *display, Window window)
{
    char *name;
    char *legacy_name = NULL;

    /* _NET_WM_NAME carries UTF-8 titles; WM_NAME remains the legacy fallback. */
    name = window_property_string(display, window, "_NET_WM_NAME");
    if (!name && XFetchName(display, window, &legacy_name) && legacy_name) {
        name = strdup(legacy_name);
        XFree(legacy_name);
    }
    return name;
}

static void load_window_metadata(Display *display, Window window,
        struct window_metadata *metadata)
{
    XClassHint class_hint;

    memset(metadata, 0, sizeof(*metadata));
    memset(&class_hint, 0, sizeof(class_hint));
    metadata->title = window_name(display, window);
    metadata->app_id = window_property_string(display, window, "_MSYS_APP_ID");
    if (!metadata->app_id)
        metadata->app_id = window_property_string(display, window,
                "_GTK_APPLICATION_ID");
    if (!metadata->app_id)
        metadata->app_id = window_property_string(display, window,
                "_KDE_NET_WM_DESKTOP_FILE");
    metadata->component_id = window_property_string(display, window,
            "_MSYS_COMPONENT_ID");
    metadata->role = window_property_string(display, window,
            "_MSYS_WINDOW_ROLE");
    if (!metadata->role)
        metadata->role = window_property_string(display, window,
                "WM_WINDOW_ROLE");
    if (XGetClassHint(display, window, &class_hint)) {
        metadata->wm_instance = class_hint.res_name;
        metadata->wm_class = class_hint.res_class;
    }
    /* The supervisor injects these variables for every framework.  Reading the
     * client process environment makes that manifest identity effective even
     * when a toolkit does not mirror it into WM_CLASS or an app-id property. */
    load_process_environment(display, window, metadata);
}

static void free_window_metadata(struct window_metadata *metadata)
{
    if (metadata->title)
        free(metadata->title);
    if (metadata->wm_instance)
        XFree(metadata->wm_instance);
    if (metadata->wm_class)
        XFree(metadata->wm_class);
    free(metadata->app_id);
    free(metadata->component_id);
    free(metadata->process_app_id);
    free(metadata->process_component_id);
    free(metadata->declared_identity);
    free(metadata->role);
    memset(metadata, 0, sizeof(*metadata));
}

static int metadata_matches_identity(const struct window_metadata *metadata,
        const char *identity)
{
    return text_equal(metadata->app_id, identity) ||
        text_equal(metadata->component_id, identity) ||
        text_equal(metadata->process_app_id, identity) ||
        text_equal(metadata->process_component_id, identity) ||
        text_equal(metadata->declared_identity, identity) ||
        text_equal(metadata->wm_instance, identity) ||
        text_equal(metadata->wm_class, identity);
}

static int metadata_matches_identity_n(const struct window_metadata *metadata,
        const char *identity, size_t identity_length)
{
    return text_equal_n(metadata->app_id, identity, identity_length) ||
        text_equal_n(metadata->component_id, identity, identity_length) ||
        text_equal_n(metadata->process_app_id, identity, identity_length) ||
        text_equal_n(metadata->process_component_id, identity, identity_length) ||
        text_equal_n(metadata->declared_identity, identity, identity_length) ||
        text_equal_n(metadata->wm_instance, identity, identity_length) ||
        text_equal_n(metadata->wm_class, identity, identity_length);
}

static int configured_identity_matches(const struct window_metadata *metadata,
        const char *environment)
{
    const char *cursor = getenv(environment);

    while (cursor && *cursor) {
        const char *start;
        const char *end;

        while (*cursor == ',' || *cursor == ' ' || *cursor == '\t')
            cursor++;
        start = cursor;
        while (*cursor && *cursor != ',')
            cursor++;
        end = cursor;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if (end > start && metadata_matches_identity_n(metadata, start,
                    (size_t)(end - start)))
            return 1;
        if (*cursor == ',')
            cursor++;
    }
    return 0;
}

static int role_kind(const char *role, enum window_kind *kind)
{
    if (!role)
        return 0;
    if (strncasecmp(role, "role:", 5) == 0)
        role += 5;
    if (text_equal(role, "launcher") || text_equal(role, "home"))
        *kind = WINDOW_LAUNCHER;
    else if (text_equal(role, "input-method") ||
            text_equal(role, "virtual-keyboard") ||
            text_equal(role, "on-screen-keyboard") ||
            text_equal(role, "soft-keyboard"))
        *kind = WINDOW_INPUT_METHOD;
    else if (text_equal(role, "task-switcher") || text_equal(role, "recents") ||
            text_equal(role, "notification-center"))
        *kind = WINDOW_RECENTS;
    else if (text_equal(role, "chooser") || text_equal(role, "intent-chooser") ||
            text_equal(role, "dialog"))
        *kind = WINDOW_CHOOSER;
    else if (text_equal(role, "notification-presenter") ||
            text_equal(role, "notification") || text_equal(role, "notifications"))
        *kind = WINDOW_NOTIFICATION;
    else if (text_equal(role, "system-chrome") || text_equal(role, "chrome") ||
            text_equal(role, "status-bar"))
        *kind = WINDOW_CHROME;
    else if (text_equal(role, "navigation-bar") ||
            text_equal(role, "navigation") || text_equal(role, "navigation-pill"))
        *kind = WINDOW_NAVIGATION;
    else if (text_equal(role, "transition-presenter") ||
            text_equal(role, "transition") || text_equal(role, "animation-mask"))
        *kind = WINDOW_TRANSITION;
    else if (text_equal(role, "screen-shield") || text_equal(role, "shield") ||
            text_equal(role, "lock-screen"))
        *kind = WINDOW_SHIELD;
    else if (text_equal(role, "application") || text_equal(role, "app"))
        *kind = WINDOW_APPLICATION;
    else
        return 0;
    return 1;
}

static enum window_kind classify_window(const struct window_metadata *metadata)
{
    enum window_kind kind;
    size_t i;

    /* An explicit role is authoritative even if a presentation title changes. */
    if (role_kind(metadata->role, &kind))
        return kind;
    for (i = 0; i < sizeof(default_identity_rules) /
            sizeof(default_identity_rules[0]); i++) {
        if (metadata_matches_identity(metadata, default_identity_rules[i].identity))
            return default_identity_rules[i].kind;
    }
    for (i = 0; i < sizeof(configured_roles) / sizeof(configured_roles[0]); i++) {
        if (configured_identity_matches(metadata, configured_roles[i].environment))
            return configured_roles[i].kind;
    }

    /* Compatibility for clients from before manifest window identity existed. */
    if (starts_with(metadata->title, "MSYS Recents"))
        return WINDOW_RECENTS;
    if (starts_with(metadata->title, "MSYS Notification Center"))
        return WINDOW_RECENTS;
    if (starts_with(metadata->title, "MSYS Intent Chooser"))
        return WINDOW_CHOOSER;
    if (starts_with(metadata->title, "MSYS Notifications"))
        return WINDOW_NOTIFICATION;
    if (starts_with(metadata->title, "MSYS Chrome"))
        return WINDOW_CHROME;
    if (starts_with(metadata->title, "MSYS Navigation"))
        return WINDOW_NAVIGATION;
    if (starts_with(metadata->title, "MSYS Transition"))
        return WINDOW_TRANSITION;
    if (starts_with(metadata->title, "MSYS Screen Shield"))
        return WINDOW_SHIELD;
    if (starts_with(metadata->title, "MSYS Launcher"))
        return WINDOW_LAUNCHER;
    return WINDOW_APPLICATION;
}

static const char *window_kind_name(enum window_kind kind)
{
    switch (kind) {
    case WINDOW_LAUNCHER:
        return "launcher";
    case WINDOW_INPUT_METHOD:
        return "overlay";
    case WINDOW_RECENTS:
        return "overlay";
    case WINDOW_CHOOSER:
        return "overlay";
    case WINDOW_NOTIFICATION:
        return "overlay";
    case WINDOW_CHROME:
        return "system-ui";
    case WINDOW_NAVIGATION:
        return "system-ui";
    case WINDOW_TRANSITION:
        return "system-ui";
    case WINDOW_SHIELD:
        return "overlay";
    case WINDOW_APPLICATION:
    default:
        return "application";
    }
}

static int window_kind_allows_override_redirect(enum window_kind kind)
{
    return kind == WINDOW_INPUT_METHOD;
}

static const char *explicit_role_name(const char *role)
{
    if (!role)
        return NULL;
    if (strncasecmp(role, "role:", 5) == 0)
        role += 5;
    if (text_equal(role, "launcher") || text_equal(role, "home"))
        return "launcher";
    if (text_equal(role, "input-method") ||
            text_equal(role, "virtual-keyboard") ||
            text_equal(role, "on-screen-keyboard") ||
            text_equal(role, "soft-keyboard"))
        return "input-method";
    if (text_equal(role, "task-switcher") || text_equal(role, "recents"))
        return "task-switcher";
    if (text_equal(role, "notification-center"))
        return "notification-center";
    if (text_equal(role, "chooser") || text_equal(role, "intent-chooser") ||
            text_equal(role, "dialog"))
        return "chooser";
    if (text_equal(role, "notification-presenter") ||
            text_equal(role, "notification") || text_equal(role, "notifications"))
        return "notification-presenter";
    if (text_equal(role, "system-chrome") || text_equal(role, "chrome") ||
            text_equal(role, "status-bar"))
        return "system-chrome";
    if (text_equal(role, "navigation-bar") ||
            text_equal(role, "navigation") || text_equal(role, "navigation-pill"))
        return "navigation-bar";
    if (text_equal(role, "transition-presenter") ||
            text_equal(role, "transition") || text_equal(role, "animation-mask"))
        return "transition-presenter";
    if (text_equal(role, "screen-shield") || text_equal(role, "shield") ||
            text_equal(role, "lock-screen"))
        return "screen-shield";
    if (text_equal(role, "application") || text_equal(role, "app"))
        return "application";
    return NULL;
}

static const char *stable_identity_role_name(
        const struct window_metadata *metadata)
{
    if (metadata_matches_identity(metadata, "org.msys.shell.launcher"))
        return "launcher";
    if (metadata_matches_identity(metadata, "org.msys.input.touch"))
        return "input-method";
    if (metadata_matches_identity(metadata, "org.msys.shell.task-switcher"))
        return "task-switcher";
    if (metadata_matches_identity(metadata,
                "org.msys.shell.notification-center"))
        return "notification-center";
    if (metadata_matches_identity(metadata, "org.msys.shell.intent-chooser"))
        return "chooser";
    if (metadata_matches_identity(metadata, "org.msys.shell.notifications"))
        return "notification-presenter";
    if (metadata_matches_identity(metadata, "org.msys.shell.system-chrome"))
        return "system-chrome";
    if (metadata_matches_identity(metadata, "org.msys.shell.navigation") ||
            metadata_matches_identity(metadata,
                "org.msys.shell.navigation-pill"))
        return "navigation-bar";
    if (metadata_matches_identity(metadata, "org.msys.shell.transitions"))
        return "transition-presenter";
    if (metadata_matches_identity(metadata, "org.msys.shell.screen-shield"))
        return "screen-shield";
    return NULL;
}

static const char *canonical_window_role(
        const struct window_metadata *metadata, enum window_kind kind,
        int *compatibility_title)
{
    const char *role;

    *compatibility_title = 0;
    role = explicit_role_name(metadata->role);
    if (role)
        return role;
    role = stable_identity_role_name(metadata);
    if (role)
        return role;
    if (starts_with(metadata->title, "MSYS Recents"))
        role = "task-switcher";
    else if (starts_with(metadata->title, "MSYS Notification Center"))
        role = "notification-center";
    else if (starts_with(metadata->title, "MSYS Intent Chooser"))
        role = "chooser";
    else if (starts_with(metadata->title, "MSYS Notifications"))
        role = "notification-presenter";
    else if (starts_with(metadata->title, "MSYS Chrome"))
        role = "system-chrome";
    else if (starts_with(metadata->title, "MSYS Navigation"))
        role = "navigation-bar";
    else if (starts_with(metadata->title, "MSYS Transition"))
        role = "transition-presenter";
    else if (starts_with(metadata->title, "MSYS Screen Shield"))
        role = "screen-shield";
    else if (starts_with(metadata->title, "MSYS Launcher"))
        role = "launcher";
    else
        role = NULL;
    if (role) {
        *compatibility_title = 1;
        return role;
    }
    /* Configured replacement identities still receive a useful generic role;
     * canonical providers should publish MSYS_WINDOW_ROLE to distinguish the
     * two recents-layer jobs. */
    switch (kind) {
    case WINDOW_INPUT_METHOD:
        return "input-method";
    case WINDOW_RECENTS:
        return "task-switcher";
    case WINDOW_CHOOSER:
        return "chooser";
    case WINDOW_NOTIFICATION:
        return "notification-presenter";
    case WINDOW_CHROME:
        return "system-chrome";
    case WINDOW_NAVIGATION:
        return "navigation-bar";
    case WINDOW_TRANSITION:
        return "transition-presenter";
    case WINDOW_SHIELD:
        return "screen-shield";
    case WINDOW_LAUNCHER:
        return "launcher";
    case WINDOW_APPLICATION:
    default:
        return "application";
    }
}

static const char *metadata_identity(const struct window_metadata *metadata)
{
    if (metadata->declared_identity)
        return metadata->declared_identity;
    if (metadata->app_id)
        return metadata->app_id;
    if (metadata->process_app_id)
        return metadata->process_app_id;
    if (metadata->wm_class)
        return metadata->wm_class;
    return metadata->wm_instance;
}

static const char *metadata_component(const struct window_metadata *metadata)
{
    return metadata->component_id
        ? metadata->component_id
        : metadata->process_component_id;
}

static void initialize_window_session(void)
{
    struct timespec realtime = {0};
    struct timespec monotonic = {0};

    if (*window_session)
        return;
    clock_gettime(CLOCK_REALTIME, &realtime);
    clock_gettime(CLOCK_MONOTONIC, &monotonic);
    snprintf(window_session, sizeof(window_session), "%lx-%lx-%lx-%lx-%lx",
            (unsigned long)realtime.tv_sec,
            (unsigned long)realtime.tv_nsec,
            (unsigned long)monotonic.tv_sec,
            (unsigned long)monotonic.tv_nsec,
            (unsigned long)getpid());
}

static int parse_window_id(const char *text, Window *window)
{
    static const char prefix[] = WINDOW_ID_SCHEMA ":";
    const char *separator;
    char *end = NULL;
    unsigned long parsed;

    if (!text || strlen(text) >= WINDOW_ID_MAX ||
            strncmp(text, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    separator = strrchr(text + sizeof(prefix) - 1, ':');
    if (!separator || separator == text + sizeof(prefix) - 1 ||
            strncmp(separator + 1, "0x", 2) != 0)
        return 0;
    errno = 0;
    parsed = strtoul(separator + 1, &end, 16);
    if (errno || !end || *end || parsed == 0)
        return 0;
    *window = (Window)parsed;
    return (unsigned long)*window == parsed;
}

static char *ensure_window_id(Display *display, Window window)
{
    Atom property = XInternAtom(display, WINDOW_ID_PROPERTY, False);
    char *existing;
    char encoded[WINDOW_ID_MAX];
    Window parsed = None;
    int length;

    existing = window_property_string(display, window, WINDOW_ID_PROPERTY);
    if (existing && parse_window_id(existing, &parsed) && parsed == window)
        return existing;
    free(existing);

    /* Serialize first assignment so concurrent diagnostic/action helpers can
     * never return a handle that another helper immediately replaces. */
    XGrabServer(display);
    existing = window_property_string(display, window, WINDOW_ID_PROPERTY);
    if (existing && parse_window_id(existing, &parsed) && parsed == window) {
        XUngrabServer(display);
        return existing;
    }
    free(existing);
    initialize_window_session();
    length = snprintf(encoded, sizeof(encoded), "%s:%s:0x%lx",
            WINDOW_ID_SCHEMA, window_session, (unsigned long)window);
    if (length < 0 || (size_t)length >= sizeof(encoded) ||
            !set_text_property(display, window, property, encoded)) {
        XUngrabServer(display);
        return NULL;
    }
    XUngrabServer(display);
    return strdup(encoded);
}

static Window resolve_window_id(Display *display, Window root,
        const char *window_id)
{
    Window target = None;
    Window root_return = None;
    Window parent_return = None;
    Window *children = NULL;
    unsigned int count = 0;
    XWindowAttributes attributes;
    char *actual;

    if (!parse_window_id(window_id, &target) || target == root ||
            !XQueryTree(display, target, &root_return, &parent_return,
                &children, &count))
        return None;
    if (children)
        XFree(children);
    if (root_return != root || parent_return != root ||
            !XGetWindowAttributes(display, target, &attributes) ||
            attributes.class == InputOnly)
        return None;
    if (attributes.override_redirect) {
        struct window_metadata metadata;
        enum window_kind kind;

        load_window_metadata(display, target, &metadata);
        kind = classify_window(&metadata);
        free_window_metadata(&metadata);
        if (!window_kind_allows_override_redirect(kind))
            return None;
    }
    actual = window_property_string(display, target, WINDOW_ID_PROPERTY);
    if (!actual || strcmp(actual, window_id) != 0)
        target = None;
    free(actual);
    return target;
}

static enum window_layer layer_for_kind(enum window_kind kind)
{
    switch (kind) {
    case WINDOW_INPUT_METHOD:
        return LAYER_INPUT_METHOD;
    case WINDOW_RECENTS:
        return LAYER_RECENTS;
    case WINDOW_CHOOSER:
        return LAYER_CHOOSER;
    case WINDOW_NOTIFICATION:
        return LAYER_NOTIFICATION;
    case WINDOW_CHROME:
        return LAYER_CHROME;
    case WINDOW_NAVIGATION:
        return LAYER_NAVIGATION;
    case WINDOW_TRANSITION:
        return LAYER_TRANSITION;
    case WINDOW_SHIELD:
        return LAYER_SHIELD;
    case WINDOW_LAUNCHER:
    case WINDOW_APPLICATION:
    default:
        return LAYER_APPLICATION;
    }
}

static enum msys_surface_kind surface_for_window_kind(enum window_kind kind)
{
    switch (kind) {
    case WINDOW_INPUT_METHOD:
        return MSYS_SURFACE_INPUT_METHOD;
    case WINDOW_CHROME:
        return MSYS_SURFACE_CHROME;
    case WINDOW_NAVIGATION:
        return MSYS_SURFACE_NAVIGATION;
    case WINDOW_NOTIFICATION:
        return MSYS_SURFACE_NOTIFICATION;
    case WINDOW_RECENTS:
        return MSYS_SURFACE_RECENTS;
    case WINDOW_CHOOSER:
        return MSYS_SURFACE_CHOOSER;
    case WINDOW_TRANSITION:
        return MSYS_SURFACE_SHIELD;
    case WINDOW_SHIELD:
        return MSYS_SURFACE_SHIELD;
    case WINDOW_LAUNCHER:
    case WINDOW_APPLICATION:
    default:
        return MSYS_SURFACE_APPLICATION;
    }
}

static int geometry_matches(const XWindowAttributes *attributes,
        const struct msys_rect *rect)
{
    return attributes && rect &&
        attributes->x == rect->x && attributes->y == rect->y &&
        attributes->width == rect->width &&
        attributes->height == rect->height;
}

static int property_affects_window_metadata(Atom property,
        const Atom *metadata_properties, size_t count)
{
    size_t index;

    if (property == None || metadata_properties == NULL)
        return 0;
    for (index = 0; index < count; index++) {
        if (metadata_properties[index] != None &&
                property == metadata_properties[index])
            return 1;
    }
    return 0;
}

static void layout_window(Display *display, Window window,
        const struct msys_layout_state *layout,
        const XConfigureRequestEvent *request)
{
    XWindowAttributes attributes;
    struct window_metadata metadata;
    enum window_kind kind;
    struct msys_rect requested;
    struct msys_rect result;

    if (!XGetWindowAttributes(display, window, &attributes) ||
            attributes.class == InputOnly)
        return;

    load_window_metadata(display, window, &metadata);
    kind = classify_window(&metadata);
    if (attributes.override_redirect &&
            !window_kind_allows_override_redirect(kind)) {
        free_window_metadata(&metadata);
        return;
    }
    requested.x = request && (request->value_mask & CWX)
        ? request->x
        : attributes.x;
    requested.y = request && (request->value_mask & CWY)
        ? request->y
        : attributes.y;
    requested.width = request && (request->value_mask & CWWidth)
        ? request->width
        : attributes.width;
    requested.height = request && (request->value_mask & CWHeight)
        ? request->height
        : attributes.height;
    msys_layout_place(layout, surface_for_window_kind(kind), &requested,
            &result);
    /* Re-sending identical geometry causes real Configure/Expose traffic in
     * Tk, Qt and Electron.  On the single-bbox SPI path that tiny metadata
     * event can then be merged with a distant key press into a near-full
     * refresh.  Stacking is handled explicitly by raise_system_overlays(). */
    if (!geometry_matches(&attributes, &result)) {
        XMoveResizeWindow(display, window, result.x, result.y,
                (unsigned int)result.width, (unsigned int)result.height);
    }
    free_window_metadata(&metadata);
}

static void watch_window_metadata(Display *display, Window window)
{
    XWindowAttributes attributes;
    char *window_id;

    if (!XGetWindowAttributes(display, window, &attributes) ||
            attributes.class == InputOnly)
        return;
    if (attributes.override_redirect) {
        struct window_metadata metadata;
        enum window_kind kind;

        load_window_metadata(display, window, &metadata);
        kind = classify_window(&metadata);
        free_window_metadata(&metadata);
        if (!window_kind_allows_override_redirect(kind))
            return;
    }
    /* Event selections are per X client, so this does not replace the
     * application's own mask. */
    XSelectInput(display, window, PropertyChangeMask);
    window_id = ensure_window_id(display, window);
    free(window_id);
}

static void raise_system_overlays(Display *display, Window root)
{
    static const enum window_layer overlay_layers[] = {
        LAYER_INPUT_METHOD,
        LAYER_RECENTS,
        LAYER_CHOOSER,
        LAYER_NOTIFICATION,
        LAYER_CHROME,
        LAYER_NAVIGATION,
        LAYER_TRANSITION,
        LAYER_SHIELD
    };
    struct layered_window {
        Window window;
        enum window_layer layer;
    };
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    struct layered_window *layered = NULL;
    unsigned int count = 0;
    unsigned int layered_count = 0;
    unsigned int i;
    size_t layer_index;

    if (!XQueryTree(display, root, &root_return, &parent_return, &children,
                &count))
        return;

    if (count > 0)
        layered = calloc(count, sizeof(*layered));
    if (count > 0 && !layered) {
        XFree(children);
        return;
    }

    for (i = 0; i < count; i++) {
        XWindowAttributes attributes;
        struct window_metadata metadata;
        enum window_kind kind;
        enum window_layer layer;

        if (!XGetWindowAttributes(display, children[i], &attributes) ||
                attributes.map_state != IsViewable ||
                attributes.class == InputOnly)
            continue;
        load_window_metadata(display, children[i], &metadata);
        kind = classify_window(&metadata);
        if (attributes.override_redirect &&
                !window_kind_allows_override_redirect(kind)) {
            free_window_metadata(&metadata);
            continue;
        }
        layer = layer_for_kind(kind);
        free_window_metadata(&metadata);
        if (layer == LAYER_APPLICATION)
            continue;
        layered[layered_count].window = children[i];
        layered[layered_count].layer = layer;
        layered_count++;
    }

    /* Preserve relative order inside a layer and impose a deterministic order
     * between layers.  Unlike the old implementation this also handles more
     * than one notification or system window at a time. */
    for (layer_index = 0; layer_index < sizeof(overlay_layers) /
            sizeof(overlay_layers[0]); layer_index++) {
        for (i = 0; i < layered_count; i++) {
            if (layered[i].layer == overlay_layers[layer_index])
                XRaiseWindow(display, layered[i].window);
        }
    }
    free(layered);
    if (children)
        XFree(children);
}

static void manage_existing(Display *display, Window root,
        const struct msys_layout_state *layout)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;
    unsigned int i;

    if (!XQueryTree(display, root, &root_return, &parent_return, &children,
                &count))
        return;
    for (i = 0; i < count; i++) {
        XWindowAttributes attributes;

        if (XGetWindowAttributes(display, children[i], &attributes) &&
                attributes.map_state == IsViewable) {
            watch_window_metadata(display, children[i]);
            layout_window(display, children[i], layout, NULL);
        }
    }
    raise_system_overlays(display, root);
    if (children)
        XFree(children);
}

static Window find_window(Display *display, Window root, const char *identity,
        const char *fallback_title)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;
    unsigned int i;
    Window target = None;

    if (!XQueryTree(display, root, &root_return, &parent_return, &children,
                &count))
        return None;

    /* XQueryTree returns children bottom-to-top, so search backwards. A
     * visible or explicitly minimized surface is eligible. Withdrawn Tk role
     * hosts remain excluded even when they share the visible Toplevel's
     * WM_CLASS. */
    if (identity && *identity) {
        for (i = count; i > 0; i--) {
            XWindowAttributes attributes;
            struct window_metadata metadata;

            if (!XGetWindowAttributes(display, children[i - 1], &attributes) ||
                    (attributes.map_state != IsViewable &&
                     window_wm_state(display, children[i - 1]) != IconicState) ||
                    attributes.class == InputOnly)
                continue;
            load_window_metadata(display, children[i - 1], &metadata);
            if (attributes.override_redirect &&
                    !window_kind_allows_override_redirect(
                        classify_window(&metadata))) {
                free_window_metadata(&metadata);
                continue;
            }
            if (metadata_matches_identity(&metadata, identity))
                target = children[i - 1];
            free_window_metadata(&metadata);
            if (target != None)
                break;
        }
    }
    if (target == None && fallback_title && *fallback_title) {
        for (i = count; i > 0; i--) {
            XWindowAttributes attributes;
            struct window_metadata metadata;

            if (!XGetWindowAttributes(display, children[i - 1], &attributes) ||
                    (attributes.map_state != IsViewable &&
                     window_wm_state(display, children[i - 1]) != IconicState) ||
                    attributes.class == InputOnly)
                continue;
            load_window_metadata(display, children[i - 1], &metadata);
            if (attributes.override_redirect &&
                    !window_kind_allows_override_redirect(
                        classify_window(&metadata))) {
                free_window_metadata(&metadata);
                continue;
            }
            if (starts_with(metadata.title, fallback_title))
                target = children[i - 1];
            free_window_metadata(&metadata);
            if (target != None)
                break;
        }
    }
    if (children)
        XFree(children);
    return target;
}

static Display *open_action_display(const char *display_name)
{
    Display *display = XOpenDisplay(display_name);

    if (!display)
        fprintf(stderr, "msys-x11-policy: cannot open DISPLAY=%s\n",
                display_name ? display_name : "");
    else
        XSetErrorHandler(handle_xerror);
    return display;
}

static void print_json_string(FILE *stream, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");

    fputc('"', stream);
    while (*cursor) {
        unsigned char byte = *cursor++;

        switch (byte) {
        case '"':
            fputs("\\\"", stream);
            break;
        case '\\':
            fputs("\\\\", stream);
            break;
        case '\b':
            fputs("\\b", stream);
            break;
        case '\f':
            fputs("\\f", stream);
            break;
        case '\n':
            fputs("\\n", stream);
            break;
        case '\r':
            fputs("\\r", stream);
            break;
        case '\t':
            fputs("\\t", stream);
            break;
        default:
            if (byte < 0x20)
                fprintf(stream, "\\u%04x", (unsigned int)byte);
            else
                fputc((int)byte, stream);
            break;
        }
    }
    fputc('"', stream);
}

static long window_wm_state(Display *display, Window window)
{
    Atom property = XInternAtom(display, "WM_STATE", True);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *value = NULL;
    long state = WithdrawnState;

    if (property != None && XGetWindowProperty(display, window, property, 0, 2,
                False, property, &actual_type, &actual_format, &item_count,
                &bytes_after, &value) == Success && value &&
            actual_type == property && actual_format == 32 && item_count >= 1)
        state = *(long *)value;
    (void)bytes_after;
    if (value)
        XFree(value);
    return state;
}

static int set_window_wm_state(Display *display, Window window, long state)
{
    Atom property = XInternAtom(display, "WM_STATE", False);
    long values[2] = {state, None};

    XChangeProperty(display, window, property, property, 32, PropModeReplace,
            (const unsigned char *)values, 2);
    return 1;
}

static const char *window_state_name(Display *display, Window window,
        const XWindowAttributes *attributes)
{
    if (attributes->map_state == IsViewable)
        return "visible";
    if (window_wm_state(display, window) == IconicState)
        return "minimized";
    return "hidden";
}

static unsigned int thumbnail_channel(unsigned long pixel, unsigned long mask)
{
    unsigned int shift = 0;
    unsigned long maximum;
    unsigned long value;

    if (mask == 0)
        return 0;
    while ((mask & 1UL) == 0UL) {
        mask >>= 1;
        shift++;
    }
    maximum = mask;
    value = (pixel >> shift) & maximum;
    if (maximum == 0)
        return 0;
    return (unsigned int)((value * 255UL + maximum / 2UL) / maximum);
}

static void thumbnail_dimensions(int width, int height, int *output_width,
        int *output_height)
{
    int scaled_width = width;
    int scaled_height = height;

    if ((long long)width * THUMBNAIL_MAX_HEIGHT >
            (long long)height * THUMBNAIL_MAX_WIDTH) {
        if (scaled_width > THUMBNAIL_MAX_WIDTH) {
            scaled_width = THUMBNAIL_MAX_WIDTH;
            scaled_height = (int)((long long)height * scaled_width / width);
        }
    } else if (scaled_height > THUMBNAIL_MAX_HEIGHT) {
        scaled_height = THUMBNAIL_MAX_HEIGHT;
        scaled_width = (int)((long long)width * scaled_height / height);
    }
    if (scaled_width < 1)
        scaled_width = 1;
    if (scaled_height < 1)
        scaled_height = 1;
    *output_width = scaled_width;
    *output_height = scaled_height;
}

static int thumbnail_directory_path(char *path, size_t capacity)
{
    const char *runtime = getenv("MSYS_RUNTIME_DIR");
    struct stat status;
    int written;

    if (!runtime || runtime[0] != '/' || strstr(runtime, "/../") ||
            strcmp(runtime, "/..") == 0)
        runtime = "/tmp/msys-main";
    written = snprintf(path, capacity, "%s/%s", runtime,
            THUMBNAIL_DIRECTORY);
    if (written < 0 || (size_t)written >= capacity)
        return 0;
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return 0;
    if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode))
        return 0;
    return 1;
}

static int thumbnail_path(Window window, char *path, size_t capacity)
{
    char directory[PATH_MAX];
    int written;

    if (!thumbnail_directory_path(directory, sizeof(directory)))
        return 0;
    written = snprintf(path, capacity, "%s/x11-%lx.ppm", directory,
            (unsigned long)window);
    return written >= 0 && (size_t)written < capacity;
}

static int thumbnail_file_available(const char *path)
{
    struct stat status;

    return lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_size > 3 && (unsigned long long)status.st_size <=
        THUMBNAIL_MAX_FILE_BYTES;
}

static int thumbnail_refresh_allowed(int map_state,
        int task_switcher_visible, int task_surface_above)
{
    /* The task switcher asks for one snapshot before mapping itself.  Once it
     * is visible, refreshing from the live root stack can replace that clean
     * preview with pixels covered by the overview.  Without an X compositor,
     * XGetImage also returns undefined (commonly black) pixels for a window
     * covered by another application or the launcher.  Refresh only the
     * topmost task surface and preserve the last visible snapshot below it. */
    return map_state == IsViewable && !task_switcher_visible &&
        !task_surface_above;
}

static int write_window_thumbnail(Display *display, Window window,
        const XWindowAttributes *attributes, const char *path)
{
    XImage *image;
    unsigned char *row = NULL;
    char temporary[PATH_MAX];
    int descriptor = -1;
    FILE *stream = NULL;
    int width;
    int height;
    int y;
    int ok = 0;

    if (attributes->map_state != IsViewable || attributes->width < 1 ||
            attributes->height < 1 || attributes->width > 32767 ||
            attributes->height > 32767)
        return 0;
    image = XGetImage(display, window, 0, 0,
            (unsigned int)attributes->width,
            (unsigned int)attributes->height, AllPlanes, ZPixmap);
    if (!image || image->red_mask == 0 || image->green_mask == 0 ||
            image->blue_mask == 0) {
        if (image)
            XDestroyImage(image);
        return 0;
    }
    thumbnail_dimensions(attributes->width, attributes->height, &width,
            &height);
    row = malloc((size_t)width * 3u);
    if (!row)
        goto cleanup;
    if (snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", path,
                (long)getpid()) >= (int)sizeof(temporary))
        goto cleanup;
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
            0600);
    if (descriptor < 0) {
        if (errno != EEXIST)
            goto cleanup;
        unlink(temporary);
        descriptor = open(temporary,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (descriptor < 0)
            goto cleanup;
    }
    stream = fdopen(descriptor, "wb");
    if (!stream)
        goto cleanup;
    descriptor = -1;
    if (fprintf(stream, "P6\n%d %d\n255\n", width, height) < 0)
        goto cleanup;
    for (y = 0; y < height; y++) {
        int source_y = (int)((long long)y * attributes->height / height);
        int x;

        for (x = 0; x < width; x++) {
            int source_x = (int)((long long)x * attributes->width / width);
            unsigned long pixel = XGetPixel(image, source_x, source_y);

            row[(size_t)x * 3u] = (unsigned char)thumbnail_channel(pixel,
                    image->red_mask);
            row[(size_t)x * 3u + 1u] = (unsigned char)thumbnail_channel(pixel,
                    image->green_mask);
            row[(size_t)x * 3u + 2u] = (unsigned char)thumbnail_channel(pixel,
                    image->blue_mask);
        }
        if (fwrite(row, 3u, (size_t)width, stream) != (size_t)width)
            goto cleanup;
    }
    if (fflush(stream) != 0 || fsync(fileno(stream)) != 0 ||
            fclose(stream) != 0) {
        stream = NULL;
        goto cleanup;
    }
    stream = NULL;
    if (rename(temporary, path) != 0)
        goto cleanup;
    ok = 1;

cleanup:
    if (stream)
        fclose(stream);
    if (descriptor >= 0)
        close(descriptor);
    if (!ok)
        unlink(temporary);
    free(row);
    XDestroyImage(image);
    return ok;
}

static int window_thumbnail(Display *display, Window window,
        const XWindowAttributes *attributes, int task_switcher_visible,
        int task_surface_above, char *path, size_t capacity)
{
    if (!thumbnail_path(window, path, capacity))
        return 0;
    if (thumbnail_refresh_allowed(attributes->map_state,
                task_switcher_visible, task_surface_above))
        (void)write_window_thumbnail(display, window, attributes, path);
    if (!thumbnail_file_available(path)) {
        path[0] = '\0';
        return 0;
    }
    return 1;
}

static void remove_window_thumbnail(Window window)
{
    char path[PATH_MAX];

    if (thumbnail_path(window, path, sizeof(path)))
        unlink(path);
}

static int mapped_task_switcher_exists(Display *display, Window *children,
        unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++) {
        XWindowAttributes attributes;
        struct window_metadata metadata;
        enum window_kind kind;
        const char *role;
        int compatibility_title;
        int matches;

        if (!XGetWindowAttributes(display, children[i], &attributes) ||
                attributes.map_state != IsViewable ||
                attributes.class == InputOnly)
            continue;
        load_window_metadata(display, children[i], &metadata);
        kind = classify_window(&metadata);
        if (attributes.override_redirect &&
                !window_kind_allows_override_redirect(kind)) {
            free_window_metadata(&metadata);
            continue;
        }
        role = canonical_window_role(&metadata, kind, &compatibility_title);
        matches = strcmp(role, "task-switcher") == 0;
        free_window_metadata(&metadata);
        if (matches)
            return 1;
    }
    return 0;
}

static int write_windows_json(FILE *stream, const char *display_name)
{
    Display *display;
    Window root;
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;
    unsigned int i;
    int emitted = 0;
    int task_switcher_visible;
    int task_surface_seen = 0;

    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    if (!XQueryTree(display, root, &root_return, &parent_return, &children,
                &count)) {
        XCloseDisplay(display);
        return 4;
    }
    task_switcher_visible = mapped_task_switcher_exists(display, children,
            count);
    fputs("{\"schema\":\"msys.window-list.v1\",\"windows\":[", stream);
    /* XQueryTree is bottom-to-top; the public contract is top-to-bottom. */
    for (i = count; i > 0; i--) {
        Window window = children[i - 1];
        XWindowAttributes attributes;
        struct window_metadata metadata;
        enum window_kind kind;
        const char *role;
        const char *identity;
        const char *component;
        const char *state;
        char native_id[32];
        char thumbnail[PATH_MAX] = "";
        char *window_id;
        int compatibility_title;

        if (!XGetWindowAttributes(display, window, &attributes) ||
                attributes.class == InputOnly)
            continue;
        load_window_metadata(display, window, &metadata);
        kind = classify_window(&metadata);
        if (attributes.override_redirect &&
                !window_kind_allows_override_redirect(kind)) {
            free_window_metadata(&metadata);
            continue;
        }
        if (!metadata.title && !metadata_identity(&metadata) &&
                !metadata_component(&metadata) && !metadata.role) {
            free_window_metadata(&metadata);
            continue;
        }
        window_id = ensure_window_id(display, window);
        if (!window_id) {
            free_window_metadata(&metadata);
            continue;
        }
        role = canonical_window_role(&metadata, kind, &compatibility_title);
        identity = metadata_identity(&metadata);
        component = metadata_component(&metadata);
        state = window_state_name(display, window, &attributes);
        if (kind == WINDOW_APPLICATION || kind == WINDOW_LAUNCHER) {
            (void)window_thumbnail(display, window, &attributes,
                    task_switcher_visible, task_surface_seen, thumbnail,
                    sizeof(thumbnail));
            if (attributes.map_state == IsViewable)
                task_surface_seen = 1;
        }
        snprintf(native_id, sizeof(native_id), "0x%lx", (unsigned long)window);
        if (emitted++)
            fputc(',', stream);
        fputs("{\"schema\":\"msys.window.v1\",\"id\":", stream);
        print_json_string(stream, window_id);
        fputs(",\"window_id\":", stream);
        print_json_string(stream, window_id);
        fputs(",\"native_id\":", stream);
        print_json_string(stream, native_id);
        fputs(",\"title\":", stream);
        print_json_string(stream, metadata.title);
        fputs(",\"identity\":", stream);
        if (identity)
            print_json_string(stream, identity);
        else
            fputs("null", stream);
        fputs(",\"component\":", stream);
        if (component)
            print_json_string(stream, component);
        else
            fputs("null", stream);
        fputs(",\"role\":", stream);
        print_json_string(stream, role);
        fputs(",\"kind\":", stream);
        print_json_string(stream, window_kind_name(kind));
        fputs(",\"state\":", stream);
        print_json_string(stream, state);
        fprintf(stream, ",\"compatibility_title\":%s,"
                "\"geometry\":{\"x\":%d,\"y\":%d,\"width\":%d,"
                "\"height\":%d}",
                compatibility_title ? "true" : "false",
                attributes.x, attributes.y, attributes.width,
                attributes.height);
        if (thumbnail[0]) {
            fputs(",\"thumbnail\":", stream);
            print_json_string(stream, thumbnail);
        }
        fputs(",\"source\":\"x11\"}", stream);
        free(window_id);
        free_window_metadata(&metadata);
    }
    fputs("]}\n", stream);
    if (children)
        XFree(children);
    XCloseDisplay(display);
    return 0;
}

static int print_windows_action(const char *display_name)
{
    return write_windows_json(stdout, display_name);
}

char *msys_x11_policy_list_windows_json(const char *display_name)
{
    char *json = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&json, &length);
    int result;

    if (!stream)
        return NULL;
    result = write_windows_json(stream, display_name);
    if (fclose(stream) != 0 || result != 0) {
        free(json);
        return NULL;
    }
    (void)length;
    return json;
}

static void copy_summary_text(char *target, size_t size, const char *value)
{
    if (!value)
        value = "";
    snprintf(target, size, "%s", value);
}

int msys_x11_policy_top_window(const char *display_name,
        int dismissible_only, struct msys_x11_window_summary *summary)
{
    Display *display;
    Window root;
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;
    unsigned int i;
    int found = 0;

    if (!summary)
        return -1;
    memset(summary, 0, sizeof(*summary));
    display = open_action_display(display_name);
    if (!display)
        return -1;
    root = DefaultRootWindow(display);
    if (!XQueryTree(display, root, &root_return, &parent_return, &children,
                &count)) {
        XCloseDisplay(display);
        return -1;
    }
    for (i = count; i > 0; i--) {
        Window window = children[i - 1];
        XWindowAttributes attributes;
        struct window_metadata metadata;
        enum window_kind kind;
        const char *role;
        const char *identity;
        const char *component;
        char *window_id;
        int compatibility_title;
        int dismissible;
        int content;

        if (!XGetWindowAttributes(display, window, &attributes) ||
                attributes.map_state != IsViewable ||
                attributes.class == InputOnly)
            continue;
        load_window_metadata(display, window, &metadata);
        kind = classify_window(&metadata);
        if (attributes.override_redirect &&
                !window_kind_allows_override_redirect(kind)) {
            free_window_metadata(&metadata);
            continue;
        }
        role = canonical_window_role(&metadata, kind, &compatibility_title);
        dismissible = strcmp(role, "screen-shield") == 0 ||
            strcmp(role, "chooser") == 0 ||
            strcmp(role, "notification-center") == 0 ||
            strcmp(role, "task-switcher") == 0 ||
            strcmp(role, "input-method") == 0;
        content = kind == WINDOW_APPLICATION || kind == WINDOW_LAUNCHER ||
            dismissible;
        if ((dismissible_only && !dismissible) ||
                (!dismissible_only && !content)) {
            free_window_metadata(&metadata);
            continue;
        }
        window_id = ensure_window_id(display, window);
        if (!window_id) {
            free_window_metadata(&metadata);
            continue;
        }
        identity = metadata_identity(&metadata);
        component = metadata_component(&metadata);
        copy_summary_text(summary->window_id, sizeof(summary->window_id),
                window_id);
        copy_summary_text(summary->title, sizeof(summary->title),
                metadata.title);
        copy_summary_text(summary->identity, sizeof(summary->identity),
                identity);
        copy_summary_text(summary->component, sizeof(summary->component),
                component);
        copy_summary_text(summary->role, sizeof(summary->role), role);
        copy_summary_text(summary->kind, sizeof(summary->kind),
                window_kind_name(kind));
        free(window_id);
        free_window_metadata(&metadata);
        found = 1;
        break;
    }
    if (children)
        XFree(children);
    XCloseDisplay(display);
    return found;
}

int msys_x11_policy_window_summary(const char *display_name,
        const char *window_id, struct msys_x11_window_summary *summary)
{
    Display *display;
    Window root;
    Window target;
    struct window_metadata metadata;
    enum window_kind kind;
    const char *role;
    int compatibility_title;

    if (!summary)
        return -1;
    memset(summary, 0, sizeof(*summary));
    display = open_action_display(display_name);
    if (!display)
        return -1;
    root = DefaultRootWindow(display);
    target = resolve_window_id(display, root, window_id);
    if (target == None) {
        XCloseDisplay(display);
        return 0;
    }
    load_window_metadata(display, target, &metadata);
    kind = classify_window(&metadata);
    role = canonical_window_role(&metadata, kind, &compatibility_title);
    (void)compatibility_title;
    copy_summary_text(summary->window_id, sizeof(summary->window_id),
            window_id);
    copy_summary_text(summary->title, sizeof(summary->title), metadata.title);
    copy_summary_text(summary->identity, sizeof(summary->identity),
            metadata_identity(&metadata));
    copy_summary_text(summary->component, sizeof(summary->component),
            metadata_component(&metadata));
    copy_summary_text(summary->role, sizeof(summary->role), role);
    copy_summary_text(summary->kind, sizeof(summary->kind),
            window_kind_name(kind));
    free_window_metadata(&metadata);
    XCloseDisplay(display);
    return 1;
}

static int window_action(const char *display_name, const char *action,
        const char *window_id, int x, int y, int width, int height)
{
    Display *display;
    Window root;
    Window target;
    XWindowAttributes attributes;
    int attempt;

    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    target = resolve_window_id(display, root, window_id);
    if (target == None) {
        fprintf(stderr, "msys-x11-policy: stale or missing window id\n");
        XCloseDisplay(display);
        return 3;
    }
    if (strcmp(action, "focus") == 0) {
        set_window_wm_state(display, target, NormalState);
        XMapRaised(display, target);
        XFlush(display);
        for (attempt = 0; attempt < 50; attempt++) {
            struct timespec delay = {0, 10 * 1000 * 1000};

            if (XGetWindowAttributes(display, target, &attributes) &&
                    attributes.map_state == IsViewable)
                break;
            nanosleep(&delay, NULL);
        }
        if (attempt == 50) {
            fprintf(stderr, "msys-x11-policy: window did not become viewable\n");
            XCloseDisplay(display);
            return 4;
        }
        XRaiseWindow(display, target);
        XSetInputFocus(display, target, RevertToPointerRoot, CurrentTime);
        raise_system_overlays(display, root);
    } else if (strcmp(action, "minimize") == 0) {
        set_window_wm_state(display, target, IconicState);
        XUnmapWindow(display, target);
    } else if (strcmp(action, "move") == 0) {
        XMoveWindow(display, target, x, y);
    } else if (strcmp(action, "resize") == 0) {
        XResizeWindow(display, target, (unsigned int)width,
                (unsigned int)height);
    } else if (strcmp(action, "move-resize") == 0) {
        XMoveResizeWindow(display, target, x, y, (unsigned int)width,
                (unsigned int)height);
    } else if (strcmp(action, "close") == 0) {
        if (!request_window_close(display, target)) {
            XCloseDisplay(display);
            return 4;
        }
    } else {
        XCloseDisplay(display);
        return 64;
    }
    XSync(display, False);
    XCloseDisplay(display);
    return 0;
}

static int raise_window(const char *display_name, const char *identity,
        const char *fallback_title)
{
    Display *display;
    Window root;
    Window target;
    XWindowAttributes attributes;
    int attempt;

    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    target = find_window(display, root, identity, fallback_title);
    if (target == None) {
        fprintf(stderr,
                "msys-x11-policy: window not found identity=%s title=%s\n",
                identity ? identity : "", fallback_title ? fallback_title : "");
        XCloseDisplay(display);
        return 3;
    }
    set_window_wm_state(display, target, NormalState);
    XMapRaised(display, target);
    XFlush(display);
    for (attempt = 0; attempt < 50; attempt++) {
        struct timespec delay = {0, 10 * 1000 * 1000};

        if (XGetWindowAttributes(display, target, &attributes) &&
                attributes.map_state == IsViewable)
            break;
        nanosleep(&delay, NULL);
    }
    if (attempt == 50) {
        fprintf(stderr, "msys-x11-policy: window did not become viewable\n");
        XCloseDisplay(display);
        return 4;
    }
    XRaiseWindow(display, target);
    XSetInputFocus(display, target, RevertToPointerRoot, CurrentTime);
    raise_system_overlays(display, root);
    XSync(display, False);
    XCloseDisplay(display);
    return 0;
}

static int request_window_close(Display *display, Window target)
{
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom *supported = NULL;
    int supported_count = 0;
    int i;
    int can_delete = 0;

    if (XGetWMProtocols(display, target, &supported, &supported_count)) {
        for (i = 0; i < supported_count; i++) {
            if (supported[i] == delete_window) {
                can_delete = 1;
                break;
            }
        }
    }
    if (supported)
        XFree(supported);
    if (can_delete) {
        XEvent event;

        memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.window = target;
        event.xclient.message_type = protocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = (long)delete_window;
        event.xclient.data.l[1] = (long)CurrentTime;
        return XSendEvent(display, target, False, NoEventMask, &event) != 0;
    }
    XKillClient(display, target);
    return 1;
}

static int close_window(const char *display_name, const char *identity,
        const char *fallback_title)
{
    Display *display;
    Window root;
    Window target;

    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    target = find_window(display, root, identity, fallback_title);
    if (target == None) {
        fprintf(stderr,
                "msys-x11-policy: window not found identity=%s title=%s\n",
                identity ? identity : "", fallback_title ? fallback_title : "");
        XCloseDisplay(display);
        return 3;
    }
    if (!request_window_close(display, target)) {
        fprintf(stderr, "msys-x11-policy: failed to close window=0x%lx\n",
                (unsigned long)target);
        XCloseDisplay(display);
        return 4;
    }
    XSync(display, False);
    XCloseDisplay(display);
    return 0;
}

static int parse_coordinate(const char *value, int *result)
{
    char *end = NULL;
    long parsed;

    if (!value || *value < '0' || *value > '9')
        return 0;
    parsed = strtol(value, &end, 10);
    if (!end || *end || parsed < 0 || parsed > 32767)
        return 0;
    *result = (int)parsed;
    return 1;
}

static int parse_signed_coordinate(const char *value, int *result)
{
    char *end = NULL;
    long parsed;

    if (!value || !*value)
        return 0;
    parsed = strtol(value, &end, 10);
    if (!end || end == value || *end || parsed < -32768 || parsed > 32767)
        return 0;
    *result = (int)parsed;
    return 1;
}

static int parse_dimension(const char *value, int *result)
{
    return parse_coordinate(value, result) && *result >= 1;
}

static int parse_gesture_duration(const char *value, int *result)
{
    return parse_coordinate(value, result) && *result >= 1 &&
        *result <= DEBUG_GESTURE_MAX_MS;
}

static int gesture_step_count(int duration_ms)
{
    int steps = (duration_ms + DEBUG_GESTURE_FRAME_MS - 1) /
        DEBUG_GESTURE_FRAME_MS;

    return steps > 0 ? steps : 1;
}

static int interpolate_coordinate(int start, int end, int step, int steps)
{
    long long distance = (long long)end - start;

    return start + (int)(distance * step / steps);
}

static enum debug_gesture_selector debug_gesture_selector_for_option(
        const char *option)
{
    if (!option)
        return DEBUG_GESTURE_NONE;
    if (strcmp(option, "--debug-swipe-identity") == 0 ||
            strcmp(option, "--debug-drag-identity") == 0)
        return DEBUG_GESTURE_IDENTITY;
    if (strcmp(option, "--debug-swipe-title") == 0 ||
            strcmp(option, "--debug-drag-title") == 0)
        return DEBUG_GESTURE_TITLE;
    if (strcmp(option, "--debug-swipe-window") == 0 ||
            strcmp(option, "--debug-drag-window") == 0)
        return DEBUG_GESTURE_WINDOW;
    return DEBUG_GESTURE_NONE;
}

static int valid_debug_gesture_target(const char *identity, const char *title)
{
    if (!identity && !title)
        return 0;
    if (identity && (!*identity || strlen(identity) > MAX_IDENTITY_BYTES))
        return 0;
    if (title && (!*title || strlen(title) > MAX_DEBUG_TITLE_BYTES))
        return 0;
    return 1;
}

static int debug_xtest_open(Display *display, struct debug_xtest_api *api)
{
    int event_base;
    int error_base;
    int major;
    int minor;

    memset(api, 0, sizeof(*api));
    api->library = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!api->library)
        api->library = dlopen("libXtst.so", RTLD_NOW | RTLD_LOCAL);
    if (!api->library)
        return 0;
    api->query_extension = dlsym(api->library, "XTestQueryExtension");
    api->fake_motion = dlsym(api->library, "XTestFakeMotionEvent");
    api->fake_button = dlsym(api->library, "XTestFakeButtonEvent");
    if (!api->query_extension || !api->fake_motion || !api->fake_button) {
        dlclose(api->library);
        memset(api, 0, sizeof(*api));
        return 0;
    }
    /*
     * XTestQueryExtension may install an Xlib close-display hook whose code
     * lives in libXtst, including when the server does not expose XTEST.  Keep
     * the library resident until its Display has been closed.
     */
    if (!api->query_extension(display, &event_base, &error_base, &major,
                &minor))
        return 0;
    return 1;
}

static void debug_xtest_close(struct debug_xtest_api *api)
{
    if (api->library)
        dlclose(api->library);
    memset(api, 0, sizeof(*api));
}

static void debug_gesture_close(Display *display, struct debug_xtest_api *api)
{
    /* libXtst must remain loaded while Xlib invokes close-display hooks. */
    if (display)
        XCloseDisplay(display);
    debug_xtest_close(api);
}

static int send_swipe(const char *display_name, const char *identity,
        const char *fallback_title, int start_x, int start_y, int end_x,
        int end_y, int duration_ms)
{
    Display *display;
    Window root;
    Window target = None;
    Window child = None;
    XWindowAttributes attributes;
    struct debug_xtest_api xtest = {0};
    int root_start_x;
    int root_start_y;
    int root_end_x;
    int root_end_y;
    int steps;
    int step;
    int pressed = 0;
    int result = 0;
    long long interval_ns;

    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    for (step = 0; step < 100 && target == None; step++) {
        struct timespec delay = {0, 50 * 1000 * 1000};

        target = find_window(display, root, identity, fallback_title);
        if (target == None)
            nanosleep(&delay, NULL);
    }
    if (target == None) {
        fprintf(stderr,
                "msys-x11-policy: gesture target not found identity=%s title=%s\n",
                identity ? identity : "", fallback_title ? fallback_title : "");
        result = 3;
        goto done;
    }
    if (!XGetWindowAttributes(display, target, &attributes) ||
            attributes.map_state != IsViewable || start_x >= attributes.width ||
            start_y >= attributes.height || end_x >= attributes.width ||
            end_y >= attributes.height) {
        fprintf(stderr,
                "msys-x11-policy: gesture coordinates exceed the visible target\n");
        result = 64;
        goto done;
    }
    if (!XTranslateCoordinates(display, target, root, start_x, start_y,
                &root_start_x, &root_start_y, &child) ||
            !XTranslateCoordinates(display, target, root, end_x, end_y,
                &root_end_x, &root_end_y, &child)) {
        result = 4;
        goto done;
    }
    if (root_start_x < 0 || root_start_y < 0 || root_end_x < 0 ||
            root_end_y < 0 || root_start_x >= DisplayWidth(display,
                DefaultScreen(display)) || root_end_x >= DisplayWidth(display,
                DefaultScreen(display)) || root_start_y >= DisplayHeight(display,
                DefaultScreen(display)) || root_end_y >= DisplayHeight(display,
                DefaultScreen(display))) {
        fprintf(stderr,
                "msys-x11-policy: gesture endpoints exceed the X11 root\n");
        result = 64;
        goto done;
    }
    if (!debug_xtest_open(display, &xtest)) {
        fprintf(stderr,
                "msys-x11-policy: XTEST extension/runtime is unavailable\n");
        result = 6;
        goto done;
    }
    if (!xtest.fake_motion(display, DefaultScreen(display), root_start_x,
                root_start_y, 0) ||
            !xtest.fake_button(display, Button1, True, 0)) {
        result = 5;
        goto xtest_done;
    }
    pressed = 1;
    XFlush(display);
    steps = gesture_step_count(duration_ms);
    interval_ns = (long long)duration_ms * 1000 * 1000 / steps;
    for (step = 1; step <= steps; step++) {
        struct timespec delay;
        int x;
        int y;

        delay.tv_sec = (time_t)(interval_ns / (1000 * 1000 * 1000));
        delay.tv_nsec = (long)(interval_ns % (1000 * 1000 * 1000));
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
            /* Continue sleeping for the unslept interval. */
        }
        x = interpolate_coordinate(root_start_x, root_end_x, step, steps);
        y = interpolate_coordinate(root_start_y, root_end_y, step, steps);
        if (!xtest.fake_motion(display, DefaultScreen(display), x, y, 0)) {
            result = 5;
            break;
        }
        XFlush(display);
    }
    if (!xtest.fake_button(display, Button1, False, 0) && result == 0)
        result = 5;
    pressed = 0;
    XSync(display, False);

xtest_done:
    if (pressed) {
        xtest.fake_button(display, Button1, False, 0);
        XSync(display, False);
    }
done:
    debug_gesture_close(display, &xtest);
    return result;
}

static Window deepest_child_at(Display *display, Window parent, int *x, int *y)
{
    int depth;

    for (depth = 0; depth < 32; depth++) {
        Window root_return;
        Window parent_return;
        Window *children = NULL;
        unsigned int count = 0;
        unsigned int i;
        Window selected = None;

        if (!XQueryTree(display, parent, &root_return, &parent_return,
                    &children, &count))
            break;
        for (i = count; i > 0; i--) {
            XWindowAttributes attributes;

            if (!XGetWindowAttributes(display, children[i - 1], &attributes) ||
                    attributes.map_state != IsViewable ||
                    attributes.class == InputOnly)
                continue;
            if (*x >= attributes.x && *y >= attributes.y &&
                    *x < attributes.x + attributes.width &&
                    *y < attributes.y + attributes.height) {
                int child_x;
                int child_y;
                Window ignored = None;

                selected = children[i - 1];
                if (XTranslateCoordinates(display, parent, selected, *x, *y,
                            &child_x, &child_y, &ignored)) {
                    *x = child_x;
                    *y = child_y;
                } else {
                    selected = None;
                }
                break;
            }
        }
        if (children)
            XFree(children);
        if (selected == None)
            break;
        parent = selected;
    }
    return parent;
}

/* Development-only input injection used by msys-tools to exercise the real
 * Tk pointer binding without xdotool/XTest or a target package install. */
static int send_click(const char *display_name, const char *identity,
        const char *fallback_title, int x, int y)
{
    Display *display;
    Window root;
    Window target;
    Window event_target;
    Window child = None;
    int root_x = 0;
    int root_y = 0;
    int event_x;
    int event_y;
    XEvent event;
    int attempts;

    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    target = None;
    for (attempts = 0; attempts < 100 && target == None; attempts++) {
        struct timespec delay = {0, 50 * 1000 * 1000};

        target = find_window(display, root, identity, fallback_title);
        if (target == None)
            nanosleep(&delay, NULL);
    }
    if (target == None) {
        fprintf(stderr,
                "msys-x11-policy: click target not found identity=%s title=%s\n",
                identity ? identity : "", fallback_title ? fallback_title : "");
        XCloseDisplay(display);
        return 3;
    }
    if (!XTranslateCoordinates(display, target, root, 0, 0, &root_x, &root_y,
                &child)) {
        XCloseDisplay(display);
        return 4;
    }
    event_x = x;
    event_y = y;
    event_target = deepest_child_at(display, target, &event_x, &event_y);

    memset(&event, 0, sizeof(event));
    event.xbutton.display = display;
    event.xbutton.window = event_target;
    event.xbutton.root = root;
    event.xbutton.subwindow = None;
    event.xbutton.time = CurrentTime;
    event.xbutton.x = event_x;
    event.xbutton.y = event_y;
    event.xbutton.x_root = root_x + x;
    event.xbutton.y_root = root_y + y;
    event.xbutton.same_screen = True;
    event.xbutton.button = Button1;
    event.xbutton.type = ButtonPress;
    event.xbutton.state = 0;
    if (!XSendEvent(display, event_target, False, ButtonPressMask, &event)) {
        XCloseDisplay(display);
        return 5;
    }
    XFlush(display);
    {
        struct timespec click_delay = {0, 20 * 1000 * 1000};

        nanosleep(&click_delay, NULL);
    }
    event.xbutton.type = ButtonRelease;
    event.xbutton.state = Button1Mask;
    if (!XSendEvent(display, event_target, False, ButtonReleaseMask, &event)) {
        XCloseDisplay(display);
        return 5;
    }
    XSync(display, False);
    XCloseDisplay(display);
    return 0;
}

static int send_root_size_event(const char *display_name, int width, int height)
{
    Display *display;
    Window root;
    XEvent event;

    if (width < 1 || height < 1)
        return 64;
    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    memset(&event, 0, sizeof(event));
    event.xconfigure.type = ConfigureNotify;
    event.xconfigure.display = display;
    event.xconfigure.event = root;
    event.xconfigure.window = root;
    event.xconfigure.width = width;
    event.xconfigure.height = height;
    if (!XSendEvent(display, root, False, StructureNotifyMask, &event)) {
        XCloseDisplay(display);
        return 5;
    }
    XSync(display, False);
    XCloseDisplay(display);
    return 0;
}

static int environment_nonnegative(const char *name, int fallback, int *result)
{
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (!value || !*value) {
        *result = fallback;
        return 1;
    }
    parsed = strtol(value, &end, 10);
    if (!end || end == value || *end || parsed < 0 || parsed > INT_MAX)
        return 0;
    *result = (int)parsed;
    return 1;
}

static int layout_config_from_environment(struct msys_layout_config *config)
{
    const char *profile = getenv("MSYS_LAYOUT_PROFILE");
    const char *orientation = getenv("MSYS_ORIENTATION");
    const char *insets = getenv("MSYS_INSETS");
    char legacy_insets[64];
    int top;
    int bottom;

    if (!profile || !*profile) {
        profile = getenv("MSYS_WINDOW_POLICY");
        if (!profile || !*profile)
            profile = "mobile";
    }
    if (!orientation || !*orientation)
        orientation = "auto";
    if (!insets || !*insets) {
        if (getenv("MSYS_CHROME_TOP") || getenv("MSYS_CHROME_BOTTOM")) {
            if (!environment_nonnegative("MSYS_CHROME_TOP", 0, &top) ||
                    !environment_nonnegative("MSYS_CHROME_BOTTOM", 0,
                        &bottom))
                return 0;
            if (snprintf(legacy_insets, sizeof(legacy_insets), "%d,0,%d,0",
                        top, bottom) < 0)
                return 0;
            insets = legacy_insets;
        } else {
            insets = "auto";
        }
    }
    return msys_layout_config_parse(config, profile, orientation, insets);
}

static int set_text_property(Display *display, Window window, Atom property,
        const char *text)
{
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);

    if (property == None || !text)
        return 0;
    XChangeProperty(display, window, property, utf8, 8, PropModeReplace,
            (const unsigned char *)text, (int)strlen(text));
    return 1;
}

static int publish_layout_config(Display *display, Window root,
        Atom property, const struct msys_layout_config *config)
{
    char encoded[MSYS_LAYOUT_TEXT_MAX];

    return msys_layout_config_encode(config, encoded, sizeof(encoded)) &&
        set_text_property(display, root, property, encoded);
}

static int publish_layout_effective(Display *display, Window root,
        Atom property, const struct msys_layout_state *state)
{
    char encoded[MSYS_LAYOUT_TEXT_MAX];

    return msys_layout_effective_encode(state, encoded, sizeof(encoded)) &&
        set_text_property(display, root, property, encoded);
}

static void apply_layout(Display *display, Window root,
        const struct msys_layout_config *config, int width, int height,
        Atom effective_property, struct msys_layout_state *state)
{
    msys_layout_resolve(config, width, height, state);
    publish_layout_effective(display, root, effective_property, state);
    fprintf(stdout,
            "msys-x11-policy: layout profile=%s orientation=%s "
            "size=%dx%d insets=%d,%d,%d,%d workarea=%d,%d,%d,%d "
            "navigation=%s\n",
            msys_layout_profile_name(state->config.profile),
            msys_orientation_name(state->orientation),
            state->screen_width, state->screen_height,
            state->insets.top, state->insets.right,
            state->insets.bottom, state->insets.left,
            state->workarea.x, state->workarea.y,
            state->workarea.width, state->workarea.height,
            msys_navigation_edge_name(state->navigation_edge));
    fflush(stdout);
    manage_existing(display, root, state);
}

static int parse_bounded_unsigned(const char *value, int minimum, int maximum,
        int *result)
{
    char *end = NULL;
    long parsed;

    if (!value || *value < '0' || *value > '9')
        return 0;
    parsed = strtol(value, &end, 10);
    if (!end || end == value || *end || parsed < minimum || parsed > maximum)
        return 0;
    *result = (int)parsed;
    return 1;
}

static int validate_display_matrix(const char *text)
{
    const char *cursor = text;
    int field;
    double matrix[9];
    double determinant;

    if (!text || !*text || strlen(text) >= DISPLAY_MATRIX_TEXT_MAX)
        return 0;
    for (field = 0; field < 9; field++) {
        char *end = NULL;

        matrix[field] = strtod(cursor, &end);
        if (!end || end == cursor || !isfinite(matrix[field]) ||
                matrix[field] < -1000000.0 || matrix[field] > 1000000.0)
            return 0;
        cursor = end;
        if (field < 8) {
            if (*cursor != ',')
                return 0;
            cursor++;
        }
    }
    if (*cursor != '\0' ||
            (matrix[8] >= -1e-12 && matrix[8] <= 1e-12))
        return 0;
    determinant =
        matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
        matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
        matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    return determinant < -1e-12 || determinant > 1e-12;
}

static int display_layout_signal_init(struct display_layout_signal *signal,
        const char *width, const char *height, const char *depth,
        const char *input_enabled, const char *input_matrix)
{
    int enabled;

    memset(signal, 0, sizeof(*signal));
    if (!parse_bounded_unsigned(width, 1, 65535, &signal->width) ||
            !parse_bounded_unsigned(height, 1, 65535, &signal->height) ||
            !parse_bounded_unsigned(depth, 0, 128, &signal->depth) ||
            !parse_bounded_unsigned(input_enabled, 0, 1, &enabled))
        return 0;
    signal->input_enabled = enabled;
    if ((enabled && !validate_display_matrix(input_matrix)) ||
            (!enabled && strcmp(input_matrix, "none") != 0))
        return 0;
    if (snprintf(signal->input_matrix, sizeof(signal->input_matrix), "%s",
                input_matrix) < 0)
        return 0;
    return 1;
}

static int display_layout_signal_encode(
        const struct display_layout_signal *signal, char *buffer, size_t size)
{
    int length = snprintf(buffer, size,
            DISPLAY_LAYOUT_SCHEMA
            ";screen=%d,%d,%d;input_enabled=%d;input_matrix=%s",
            signal->width, signal->height, signal->depth,
            signal->input_enabled, signal->input_matrix);

    return length >= 0 && (size_t)length < size;
}

static int display_layout_signal_decode(const char *text,
        struct display_layout_signal *signal)
{
    static const char prefix[] = DISPLAY_LAYOUT_SCHEMA ";screen=";
    char width[16];
    char height[16];
    char depth[16];
    char enabled[4];
    char matrix[DISPLAY_MATRIX_TEXT_MAX];
    char trailing;

    if (!text || strncmp(text, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    if (sscanf(text + sizeof(prefix) - 1,
                "%15[0-9],%15[0-9],%15[0-9];input_enabled=%3[0-9];"
                "input_matrix=%191[^;]%c",
                width, height, depth, enabled, matrix, &trailing) != 5)
        return 0;
    return display_layout_signal_init(signal, width, height, depth, enabled,
            matrix);
}

static int sync_display_session_action(const char *display_name,
        const char *width, const char *height, const char *depth,
        const char *input_enabled, const char *input_matrix)
{
    struct display_layout_signal signal;
    char encoded[384];
    Display *display;
    Window root;
    Atom property;

    if (!display_layout_signal_init(&signal, width, height, depth,
                input_enabled, input_matrix) ||
            !display_layout_signal_encode(&signal, encoded, sizeof(encoded))) {
        fprintf(stderr, "msys-x11-policy: invalid display-session layout signal\n");
        return 64;
    }
    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    property = XInternAtom(display, DISPLAY_LAYOUT_PROPERTY, False);
    if (!set_text_property(display, root, property, encoded)) {
        XCloseDisplay(display);
        return 4;
    }
    XSync(display, False);
    XCloseDisplay(display);
    return 0;
}

static int apply_display_session_property(Display *display, Window root,
        const struct msys_layout_config *config, Atom effective_property,
        struct msys_layout_state *state, int *width, int *height)
{
    struct display_layout_signal signal;
    XWindowAttributes attributes;
    char *encoded = window_property_string(display, root,
            DISPLAY_LAYOUT_PROPERTY);
    int applied = 0;

    if (!encoded)
        return 0;
    if (!display_layout_signal_decode(encoded, &signal)) {
        fprintf(stderr,
                "msys-x11-policy: ignored invalid display-session property\n");
        goto done;
    }
    if (!XGetWindowAttributes(display, root, &attributes))
        goto done;
    if (signal.width != attributes.width || signal.height != attributes.height ||
            (signal.depth != 0 && signal.depth != attributes.depth)) {
        fprintf(stderr,
                "msys-x11-policy: deferred display-session geometry=%dx%dx%d "
                "root=%dx%dx%d\n",
                signal.width, signal.height, signal.depth, attributes.width,
                attributes.height, attributes.depth);
        goto done;
    }
    *width = signal.width;
    *height = signal.height;
    apply_layout(display, root, config, *width, *height, effective_property,
            state);
    fprintf(stdout,
            "msys-x11-policy: display-session applied depth=%d input=%s\n",
            signal.depth,
            signal.input_enabled ? signal.input_matrix : "none");
    fflush(stdout);
    applied = 1;

done:
    free(encoded);
    return applied;
}

static int set_layout_action(const char *display_name, const char *profile,
        const char *orientation, const char *insets)
{
    struct msys_layout_config config;
    Display *display;
    Window root;
    Atom property;

    if (!msys_layout_config_parse(&config, profile, orientation, insets)) {
        fprintf(stderr,
                "msys-x11-policy: invalid layout profile=%s orientation=%s "
                "insets=%s\n",
                profile, orientation, insets);
        return 64;
    }
    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    property = XInternAtom(display, LAYOUT_CONFIG_PROPERTY, False);
    if (!publish_layout_config(display, root, property, &config)) {
        XCloseDisplay(display);
        return 4;
    }
    XSync(display, False);
    XCloseDisplay(display);
    return 0;
}

static int print_layout_action(const char *display_name)
{
    Display *display;
    Window root;
    char *effective;

    display = open_action_display(display_name);
    if (!display)
        return 1;
    root = DefaultRootWindow(display);
    effective = window_property_string(display, root,
            LAYOUT_EFFECTIVE_PROPERTY);
    if (!effective) {
        fprintf(stderr, "msys-x11-policy: effective layout is unavailable\n");
        XCloseDisplay(display);
        return 3;
    }
    puts(effective);
    free(effective);
    XCloseDisplay(display);
    return 0;
}

char *msys_x11_policy_layout_wire(const char *display_name)
{
    Display *display = open_action_display(display_name);
    char *effective;
    char *result;

    if (!display)
        return NULL;
    effective = window_property_string(display, DefaultRootWindow(display),
            LAYOUT_EFFECTIVE_PROPERTY);
    XCloseDisplay(display);
    if (!effective)
        return NULL;
    result = strdup(effective);
    free(effective);
    return result;
}

int msys_x11_policy_window_action(const char *display_name,
        const char *action, const char *window_id,
        int x, int y, int width, int height)
{
    return window_action(display_name, action, window_id,
            x, y, width, height);
}

int msys_x11_policy_activate(const char *display_name,
        const char *identity, const char *title)
{
    return raise_window(display_name,
            identity && *identity ? identity : NULL,
            title && *title ? title : NULL);
}

int msys_x11_policy_close_identity(const char *display_name,
        const char *identity, const char *title)
{
    return close_window(display_name,
            identity && *identity ? identity : NULL,
            title && *title ? title : NULL);
}

int msys_x11_policy_set_layout(const char *display_name,
        const char *profile, const char *orientation, const char *insets)
{
    return set_layout_action(display_name, profile, orientation, insets);
}

int msys_x11_policy_sync_display(const char *display_name,
        int width, int height, int depth, int input_enabled,
        const char *input_matrix)
{
    char width_text[16];
    char height_text[16];
    char depth_text[16];
    char enabled_text[4];

    snprintf(width_text, sizeof(width_text), "%d", width);
    snprintf(height_text, sizeof(height_text), "%d", height);
    snprintf(depth_text, sizeof(depth_text), "%d", depth);
    snprintf(enabled_text, sizeof(enabled_text), "%d", input_enabled);
    return sync_display_session_action(display_name, width_text, height_text,
            depth_text, enabled_text, input_matrix);
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--list-windows | --window-focus WINDOW_ID | "
            "--window-minimize WINDOW_ID | --window-close WINDOW_ID | "
            "--window-move WINDOW_ID X Y | --window-resize WINDOW_ID W H | "
            "--window-move-resize WINDOW_ID X Y W H | "
            "--raise-title TITLE | --raise-identity ID | "
            "--raise-window ID TITLE | --close-identity ID | "
            "--close-title TITLE | --close-window ID TITLE | "
            "--debug-click-identity ID X Y | --debug-click-window ID TITLE X Y | "
            "--debug-swipe-identity ID X1 Y1 X2 Y2 MS | "
            "--debug-swipe-title TITLE X1 Y1 X2 Y2 MS | "
            "--debug-swipe-window ID TITLE X1 Y1 X2 Y2 MS | "
            "(--debug-drag-* aliases are accepted) | "
            "--debug-root-size WIDTH HEIGHT | "
            "--set-layout PROFILE ORIENTATION INSETS | --print-layout | "
            "--sync-display-session WIDTH HEIGHT DEPTH ENABLED MATRIX | "
            "--publish-display-session STATE_FILE PROVIDER]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *display_name = getenv("DISPLAY");
    struct msys_layout_config layout_config;
    struct msys_layout_state layout_state;
    Display *display;
    Window root;
    Atom layout_config_property;
    Atom layout_effective_property;
    Atom display_layout_property;
    Atom metadata_properties[10];
    int screen;
    int width;
    int height;
    int click_x;
    int click_y;
    int debug_width;
    int debug_height;
    int gesture_start_x;
    int gesture_start_y;
    int gesture_end_x;
    int gesture_end_y;
    int gesture_duration;
    int action_x;
    int action_y;
    int action_width;
    int action_height;
    enum debug_gesture_selector gesture_selector;
    int allow_debug_layout;
    struct msys_x11_agent *agent = NULL;
    int agent_result = 0;
    int exit_status = 0;

    /* Native mIPC calls are handled by bounded worker threads so role calls
     * may re-enter activate_component/recents without deadlocking the private
     * channel. Each worker opens its own X connection. */
    if (!XInitThreads()) {
        fprintf(stderr, "msys-x11-policy: Xlib thread support unavailable\n");
        return 1;
    }

    if (argc == 2 && strcmp(argv[1], "--list-windows") == 0)
        return print_windows_action(display_name);
    if (argc == 3 && strcmp(argv[1], "--window-focus") == 0)
        return window_action(display_name, "focus", argv[2], 0, 0, 0, 0);
    if (argc == 3 && strcmp(argv[1], "--window-minimize") == 0)
        return window_action(display_name, "minimize", argv[2], 0, 0, 0, 0);
    if (argc == 3 && strcmp(argv[1], "--window-close") == 0)
        return window_action(display_name, "close", argv[2], 0, 0, 0, 0);
    if (argc == 5 && strcmp(argv[1], "--window-move") == 0) {
        if (!parse_signed_coordinate(argv[3], &action_x) ||
                !parse_signed_coordinate(argv[4], &action_y)) {
            print_usage(argv[0]);
            return 64;
        }
        return window_action(display_name, "move", argv[2], action_x,
                action_y, 0, 0);
    }
    if (argc == 5 && strcmp(argv[1], "--window-resize") == 0) {
        if (!parse_dimension(argv[3], &action_width) ||
                !parse_dimension(argv[4], &action_height)) {
            print_usage(argv[0]);
            return 64;
        }
        return window_action(display_name, "resize", argv[2], 0, 0,
                action_width, action_height);
    }
    if (argc == 7 && strcmp(argv[1], "--window-move-resize") == 0) {
        if (!parse_signed_coordinate(argv[3], &action_x) ||
                !parse_signed_coordinate(argv[4], &action_y) ||
                !parse_dimension(argv[5], &action_width) ||
                !parse_dimension(argv[6], &action_height)) {
            print_usage(argv[0]);
            return 64;
        }
        return window_action(display_name, "move-resize", argv[2], action_x,
                action_y, action_width, action_height);
    }
    if (argc == 3 && strcmp(argv[1], "--raise-title") == 0)
        return raise_window(display_name, NULL, argv[2]);
    if (argc == 3 && strcmp(argv[1], "--raise-identity") == 0)
        return raise_window(display_name, argv[2], NULL);
    if (argc == 4 && strcmp(argv[1], "--raise-window") == 0)
        return raise_window(display_name, argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--close-identity") == 0)
        return close_window(display_name, argv[2], NULL);
    if (argc == 3 && strcmp(argv[1], "--close-title") == 0)
        return close_window(display_name, NULL, argv[2]);
    if (argc == 4 && strcmp(argv[1], "--close-window") == 0)
        return close_window(display_name, argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "--debug-click-identity") == 0) {
        if (!parse_coordinate(argv[3], &click_x) ||
                !parse_coordinate(argv[4], &click_y)) {
            print_usage(argv[0]);
            return 64;
        }
        return send_click(display_name, argv[2], NULL, click_x, click_y);
    }
    if (argc == 6 && strcmp(argv[1], "--debug-click-window") == 0) {
        if (!parse_coordinate(argv[4], &click_x) ||
                !parse_coordinate(argv[5], &click_y)) {
            print_usage(argv[0]);
            return 64;
        }
        return send_click(display_name, argv[2], argv[3], click_x, click_y);
    }
    gesture_selector = argc > 1
        ? debug_gesture_selector_for_option(argv[1])
        : DEBUG_GESTURE_NONE;
    if (gesture_selector != DEBUG_GESTURE_NONE) {
        const char *identity = NULL;
        const char *title = NULL;
        int coordinate_index;

        if (gesture_selector == DEBUG_GESTURE_WINDOW) {
            if (argc != 9) {
                print_usage(argv[0]);
                return 64;
            }
            identity = argv[2];
            title = argv[3];
            coordinate_index = 4;
        } else {
            if (argc != 8) {
                print_usage(argv[0]);
                return 64;
            }
            if (gesture_selector == DEBUG_GESTURE_IDENTITY)
                identity = argv[2];
            else
                title = argv[2];
            coordinate_index = 3;
        }
        if (!valid_debug_gesture_target(identity, title) ||
                !parse_coordinate(argv[coordinate_index], &gesture_start_x) ||
                !parse_coordinate(argv[coordinate_index + 1],
                    &gesture_start_y) ||
                !parse_coordinate(argv[coordinate_index + 2],
                    &gesture_end_x) ||
                !parse_coordinate(argv[coordinate_index + 3],
                    &gesture_end_y) ||
                !parse_gesture_duration(argv[coordinate_index + 4],
                    &gesture_duration)) {
            print_usage(argv[0]);
            return 64;
        }
        return send_swipe(display_name, identity, title, gesture_start_x,
                gesture_start_y, gesture_end_x, gesture_end_y,
                gesture_duration);
    }
    if (argc == 4 && strcmp(argv[1], "--debug-root-size") == 0) {
        if (!parse_coordinate(argv[2], &debug_width) || debug_width < 1 ||
                !parse_coordinate(argv[3], &debug_height) || debug_height < 1) {
            print_usage(argv[0]);
            return 64;
        }
        return send_root_size_event(display_name, debug_width, debug_height);
    }
    if (argc == 5 && strcmp(argv[1], "--set-layout") == 0)
        return set_layout_action(display_name, argv[2], argv[3], argv[4]);
    if (argc == 2 && strcmp(argv[1], "--print-layout") == 0)
        return print_layout_action(display_name);
    if (argc == 7 && strcmp(argv[1], "--sync-display-session") == 0)
        return sync_display_session_action(display_name, argv[2], argv[3],
                argv[4], argv[5], argv[6]);
    if (argc == 4 && strcmp(argv[1], "--publish-display-session") == 0)
        return msys_x11_publish_display_session(display_name, argv[3],
                argv[2]);
    if (argc != 1) {
        print_usage(argv[0]);
        return 64;
    }
    if (!layout_config_from_environment(&layout_config)) {
        fprintf(stderr,
                "msys-x11-policy: invalid MSYS_LAYOUT_PROFILE / "
                "MSYS_ORIENTATION / MSYS_INSETS environment\n");
        return 64;
    }
    allow_debug_layout = getenv("MSYS_DEBUG_LAYOUT") &&
        strcmp(getenv("MSYS_DEBUG_LAYOUT"), "1") == 0;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    display = XOpenDisplay(display_name);
    if (!display) {
        fprintf(stderr, "msys-x11-policy: cannot open DISPLAY=%s\n",
                display_name ? display_name : "");
        return 1;
    }

    screen = DefaultScreen(display);
    root = RootWindow(display, screen);
    width = DisplayWidth(display, screen);
    height = DisplayHeight(display, screen);
    layout_config_property = XInternAtom(display, LAYOUT_CONFIG_PROPERTY,
            False);
    layout_effective_property = XInternAtom(display,
            LAYOUT_EFFECTIVE_PROPERTY, False);
    display_layout_property = XInternAtom(display, DISPLAY_LAYOUT_PROPERTY,
            False);
    metadata_properties[0] = XInternAtom(display, "_MSYS_WINDOW_ROLE", False);
    metadata_properties[1] = XInternAtom(display, "_MSYS_APP_ID", False);
    metadata_properties[2] = XInternAtom(display, "_MSYS_COMPONENT_ID", False);
    metadata_properties[3] = XInternAtom(display, "WM_WINDOW_ROLE", False);
    metadata_properties[4] = XA_WM_CLASS;
    metadata_properties[5] = XInternAtom(display, "_NET_WM_NAME", False);
    metadata_properties[6] = XA_WM_NAME;
    metadata_properties[7] = XInternAtom(display, "_GTK_APPLICATION_ID", False);
    metadata_properties[8] = XInternAtom(display,
            "_KDE_NET_WM_DESKTOP_FILE", False);
    metadata_properties[9] = XInternAtom(display, "_NET_WM_PID", False);

    XSetErrorHandler(handle_xerror);
    XSelectInput(display, root, SubstructureRedirectMask |
            SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);
    XSync(display, False);
    if (wm_conflict) {
        fprintf(stderr,
                "msys-x11-policy: another window manager owns DISPLAY=%s\n",
                display_name ? display_name : "");
        XCloseDisplay(display);
        return 2;
    }

    fprintf(stdout,
            "msys-x11-policy: ready display=%s size=%dx%d\n",
            display_name ? display_name : "", width, height);
    fflush(stdout);
    publish_layout_config(display, root, layout_config_property,
            &layout_config);
    apply_layout(display, root, &layout_config, width, height,
            layout_effective_property, &layout_state);
    apply_display_session_property(display, root, &layout_config,
            layout_effective_property, &layout_state, &width, &height);
    XSync(display, False);

    agent_result = msys_x11_agent_start(&agent, display_name);
    if (agent_result != 0) {
        XCloseDisplay(display);
        return agent_result;
    }

    while (running) {
        XEvent event;

        agent_result = msys_x11_agent_poll(agent);
        if (agent_result != 0) {
            exit_status = agent_result == 100 ? 0 : agent_result;
            break;
        }

        if (!XPending(display)) {
            struct timespec delay = {0, 10 * 1000 * 1000};

            nanosleep(&delay, NULL);
            continue;
        }
        XNextEvent(display, &event);
        switch (event.type) {
        case MapRequest:
            watch_window_metadata(display, event.xmaprequest.window);
            XMapWindow(display, event.xmaprequest.window);
            layout_window(display, event.xmaprequest.window, &layout_state,
                    NULL);
            raise_system_overlays(display, root);
            break;
        case ConfigureRequest: {
            XConfigureRequestEvent *request = &event.xconfigurerequest;

            layout_window(display, request->window, &layout_state, request);
            raise_system_overlays(display, root);
            break;
        }
        case MapNotify:
            watch_window_metadata(display, event.xmap.window);
            layout_window(display, event.xmap.window, &layout_state, NULL);
            raise_system_overlays(display, root);
            break;
        case PropertyNotify:
            if (event.xproperty.window == root &&
                    event.xproperty.atom == layout_config_property) {
                struct msys_layout_config requested_config;
                char *encoded = window_property_string(display, root,
                        LAYOUT_CONFIG_PROPERTY);

                if (encoded && msys_layout_config_decode(encoded,
                            &requested_config)) {
                    layout_config = requested_config;
                    apply_layout(display, root, &layout_config, width, height,
                            layout_effective_property, &layout_state);
                } else {
                    fprintf(stderr,
                            "msys-x11-policy: ignored invalid root layout "
                            "property\n");
                }
                free(encoded);
            } else if (event.xproperty.window == root &&
                    event.xproperty.atom == display_layout_property) {
                apply_display_session_property(display, root, &layout_config,
                        layout_effective_property, &layout_state, &width,
                        &height);
            } else if (event.xproperty.window != root &&
                    property_affects_window_metadata(
                        event.xproperty.atom,
                        metadata_properties,
                        sizeof(metadata_properties) /
                            sizeof(metadata_properties[0]))) {
                layout_window(display, event.xproperty.window, &layout_state,
                        NULL);
                raise_system_overlays(display, root);
            }
            break;
        case ConfigureNotify:
            if (event.xconfigure.window == root &&
                    (!event.xconfigure.send_event || allow_debug_layout) &&
                    (event.xconfigure.width != width ||
                     event.xconfigure.height != height)) {
                width = event.xconfigure.width;
                height = event.xconfigure.height;
                apply_layout(display, root, &layout_config, width, height,
                        layout_effective_property, &layout_state);
                if (!event.xconfigure.send_event)
                    apply_display_session_property(display, root,
                            &layout_config, layout_effective_property,
                            &layout_state, &width, &height);
            }
            break;
        case DestroyNotify:
            remove_window_thumbnail(event.xdestroywindow.window);
            break;
        default:
            break;
        }
        XSync(display, False);
    }

    msys_x11_agent_stop(agent);
    XCloseDisplay(display);
    return exit_status;
}
