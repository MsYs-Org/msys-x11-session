#define _POSIX_C_SOURCE 200809L

#include "msys_x11_agent.h"
#include "msys_x11_policy_api.h"

#include "msys/mipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define AGENT_COMPONENT_FALLBACK "org.msys.x11.session:window-policy"
#define AGENT_WORKER_STACK (256u * 1024u)
#define AGENT_SHUTDOWN 100

struct display_session {
    char *json;
    char provider[257];
    char display[64];
    uint64_t generation;
    int width;
    int height;
    int depth;
    int input_enabled;
    char matrix[192];
};

struct msys_x11_agent {
    msys_mipc_client client;
    pthread_mutex_t send_lock;
    pthread_mutex_t worker_lock;
    pthread_mutex_t session_lock;
    pthread_cond_t worker_idle;
    unsigned int workers;
    int stopping;
    char display[64];
    char runtime_dir[PATH_MAX];
    char state_path[PATH_MAX];
    struct stat state_stat;
    int have_state_stat;
    struct display_session session;
    uint64_t next_state_poll_ms;
    char *recv_packet;
};

struct worker_call {
    struct msys_x11_agent *agent;
    uint64_t id;
    char method[96];
    char *payload;
};

static char *format_json(const char *format, ...)
{
    va_list arguments;
    va_list copy;
    int required;
    char *result;

    va_start(arguments, format);
    va_copy(copy, arguments);
    required = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (required < 0) {
        va_end(arguments);
        return NULL;
    }
    result = malloc((size_t)required + 1u);
    if (result)
        vsnprintf(result, (size_t)required + 1u, format, arguments);
    va_end(arguments);
    return result;
}

static char *json_quote(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");
    size_t required = 3u;
    char *result;
    char *output;

    while (*cursor) {
        unsigned char byte = *cursor++;
        required += byte < 0x20 ? 6u :
            (byte == '"' || byte == '\\' ? 2u : 1u);
    }
    result = malloc(required);
    if (!result)
        return NULL;
    output = result;
    *output++ = '"';
    cursor = (const unsigned char *)(value ? value : "");
    while (*cursor) {
        static const char hexadecimal[] = "0123456789abcdef";
        unsigned char byte = *cursor++;

        if (byte == '"' || byte == '\\') {
            *output++ = '\\';
            *output++ = (char)byte;
        } else if (byte < 0x20) {
            *output++ = '\\';
            *output++ = 'u';
            *output++ = '0';
            *output++ = '0';
            *output++ = hexadecimal[byte >> 4];
            *output++ = hexadecimal[byte & 15u];
        } else {
            *output++ = (char)byte;
        }
    }
    *output++ = '"';
    *output = '\0';
    return result;
}

static char *copy_raw(const char *json, const char *key)
{
    const char *raw;
    size_t length;
    char *result;

    if (msys_mipc_json_get_raw(json, key, &raw, &length) != MSYS_MIPC_OK)
        return NULL;
    result = malloc(length + 1u);
    if (!result)
        return NULL;
    memcpy(result, raw, length);
    result[length] = '\0';
    return result;
}

static int json_get_int(const char *json, const char *key, int minimum,
        int maximum, int *value)
{
    char *raw = copy_raw(json, key);
    char *end = NULL;
    long parsed;

    if (!raw)
        return 0;
    errno = 0;
    parsed = strtol(raw, &end, 10);
    if (errno || !end || end == raw || *end || parsed < minimum ||
            parsed > maximum) {
        free(raw);
        return 0;
    }
    *value = (int)parsed;
    free(raw);
    return 1;
}

static int json_get_boolean(const char *json, const char *key, int *value)
{
    char *raw = copy_raw(json, key);
    int valid = 1;

    if (!raw)
        return 0;
    if (strcmp(raw, "true") == 0)
        *value = 1;
    else if (strcmp(raw, "false") == 0)
        *value = 0;
    else
        valid = 0;
    free(raw);
    return valid;
}

static int packet_type_is(const char *json, const char *expected)
{
    char type[32];

    return msys_mipc_json_get_string(json, "type", type, sizeof(type), NULL) ==
            MSYS_MIPC_OK && strcmp(type, expected) == 0;
}

static int valid_display_name(const char *value)
{
    const char *cursor = value;

    if (!cursor || *cursor++ != ':' || *cursor < '0' || *cursor > '9')
        return 0;
    while (*cursor >= '0' && *cursor <= '9')
        cursor++;
    if (*cursor == '.') {
        cursor++;
        if (*cursor < '0' || *cursor > '9')
            return 0;
        while (*cursor >= '0' && *cursor <= '9')
            cursor++;
    }
    return *cursor == '\0';
}

static int valid_component_name(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    if (!cursor || !*cursor)
        return 0;
    while (*cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
                (*cursor >= 'A' && *cursor <= 'Z') ||
                (*cursor >= '0' && *cursor <= '9') || *cursor == '.' ||
                *cursor == ':' || *cursor == '_' || *cursor == '-'))
            return 0;
        cursor++;
    }
    return 1;
}

static int send_return_locked(struct msys_x11_agent *agent, uint64_t id,
        const char *payload)
{
    int result;

    pthread_mutex_lock(&agent->send_lock);
    result = msys_mipc_send_return_json(&agent->client, id,
            payload ? payload : "{}");
    pthread_mutex_unlock(&agent->send_lock);
    return result;
}

static int send_event_locked(struct msys_x11_agent *agent, const char *topic,
        const char *payload)
{
    int result;

    pthread_mutex_lock(&agent->send_lock);
    result = msys_mipc_send_event_json(&agent->client, topic,
            payload ? payload : "{}");
    pthread_mutex_unlock(&agent->send_lock);
    return result;
}

static int set_socket_timeout(int fd, int timeout_ms)
{
    struct timeval timeout;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
            sizeof(timeout)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
            sizeof(timeout)) == 0;
}

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = send(fd, buffer + offset, length - offset,
                MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return 0;
        offset += (size_t)written;
    }
    return 1;
}

static char *read_line(int fd)
{
    char *buffer = malloc(MSYS_MIPC_RECV_CAPACITY);
    size_t length = 0;

    if (!buffer)
        return NULL;
    while (length < MSYS_MIPC_MAX_PACKET) {
        ssize_t count = recv(fd, buffer + length,
                MSYS_MIPC_MAX_PACKET - length, 0);
        char *newline;

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            free(buffer);
            return NULL;
        }
        length += (size_t)count;
        newline = memchr(buffer, '\n', length);
        if (newline) {
            *newline = '\0';
            return buffer;
        }
    }
    free(buffer);
    return NULL;
}

static char *public_call(struct msys_x11_agent *agent, const char *target,
        const char *method, const char *payload, int timeout_ms)
{
    struct sockaddr_un address;
    char socket_path[PATH_MAX];
    char *quoted_target = NULL;
    char *quoted_method = NULL;
    char *request = NULL;
    char *welcome = NULL;
    char *response = NULL;
    uint64_t now = 0;
    int fd = -1;

    if (snprintf(socket_path, sizeof(socket_path), "%s/control.sock",
                agent->runtime_dir) < 0 || strlen(socket_path) >=
            sizeof(address.sun_path))
        goto done;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1u);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || !set_socket_timeout(fd, timeout_ms) ||
            connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        goto done;
    welcome = read_line(fd);
    if (!welcome || !packet_type_is(welcome, "welcome"))
        goto done;
    if (msys_mipc_monotonic_ms(&now) != MSYS_MIPC_OK)
        goto done;
    quoted_target = json_quote(target);
    quoted_method = json_quote(method);
    if (!quoted_target || !quoted_method)
        goto done;
    request = format_json(
            "{\"type\":\"call\",\"id\":1,\"target\":%s,"
            "\"method\":%s,\"payload\":%s,\"deadline_ms\":%llu}\n",
            quoted_target, quoted_method, payload ? payload : "{}",
            (unsigned long long)(now + (uint64_t)timeout_ms));
    if (!request || !write_all(fd, request, strlen(request)))
        goto done;
    response = read_line(fd);

done:
    if (fd >= 0)
        close(fd);
    free(quoted_target);
    free(quoted_method);
    free(request);
    free(welcome);
    return response;
}

static char *public_payload(char *response)
{
    char *payload;

    if (!response || !packet_type_is(response, "return")) {
        free(response);
        return NULL;
    }
    payload = copy_raw(response, "payload");
    free(response);
    return payload;
}

int msys_x11_agent_active_foreground_component(const char *payload,
        char *component,
        size_t size)
{
    char *windows = copy_raw(payload, "windows");
    const char *cursor;
    int result = 0;

    if (!component || size == 0u) {
        free(windows);
        return 0;
    }
    component[0] = '\0';
    if (!windows)
        return 0;
    cursor = strchr(windows, '{');
    while (cursor) {
        const char *end = cursor;
        int depth = 0;
        int quoted = 0;
        int escaped = 0;

        while (*end) {
            char byte = *end++;
            if (quoted) {
                if (escaped)
                    escaped = 0;
                else if (byte == '\\')
                    escaped = 1;
                else if (byte == '"')
                    quoted = 0;
            } else if (byte == '"') {
                quoted = 1;
            } else if (byte == '{') {
                depth++;
            } else if (byte == '}' && --depth == 0) {
                size_t length = (size_t)(end - cursor);
                char *object = malloc(length + 1u);
                if (object) {
                    char state[32] = "";
                    memcpy(object, cursor, length);
                    object[length] = '\0';
                    (void)msys_mipc_json_get_string(object, "state", state,
                            sizeof(state), NULL);
                    if (strcmp(state, "background") != 0) {
                        result = msys_mipc_json_get_string(object,
                                "component", component, size, NULL) ==
                                MSYS_MIPC_OK && *component;
                    }
                    free(object);
                }
                break;
            }
        }
        if (result || !*end)
            break;
        cursor = strchr(end, '{');
    }
    free(windows);
    return result;
}

int msys_x11_agent_home_visible(const char *active_component,
        const char *top_role)
{
    return (!active_component || !*active_component) && top_role &&
        strcmp(top_role, "launcher") == 0;
}

static int foreground_contains_component(const char *payload,
        const char *expected)
{
    char *windows = copy_raw(payload, "windows");
    const char *cursor;
    int found = 0;

    if (!windows || !expected || !*expected) {
        free(windows);
        return 0;
    }
    cursor = strchr(windows, '{');
    while (cursor) {
        const char *end = cursor;
        int depth = 0;
        int quoted = 0;
        int escaped = 0;

        while (*end) {
            char byte = *end++;
            if (quoted) {
                if (escaped)
                    escaped = 0;
                else if (byte == '\\')
                    escaped = 1;
                else if (byte == '"')
                    quoted = 0;
            } else if (byte == '"') {
                quoted = 1;
            } else if (byte == '{') {
                depth++;
            } else if (byte == '}' && --depth == 0) {
                size_t length = (size_t)(end - cursor);
                char *object = malloc(length + 1u);
                if (object) {
                    char component[257];
                    memcpy(object, cursor, length);
                    object[length] = '\0';
                    found = msys_mipc_json_get_string(object, "component",
                            component, sizeof(component), NULL) ==
                            MSYS_MIPC_OK && strcmp(component, expected) == 0;
                    free(object);
                }
                break;
            }
        }
        if (found || !*end)
            break;
        cursor = strchr(end, '{');
    }
    free(windows);
    return found;
}

static char *foreground_component(struct msys_x11_agent *agent,
        char *component, size_t size)
{
    char *response = public_call(agent, "msys.core", "foreground_stack",
            "{}", 3000);
    char *payload = public_payload(response);

    *component = '\0';
    if (payload)
        msys_x11_agent_active_foreground_component(payload, component, size);
    return payload;
}

static char *layout_json(struct msys_x11_agent *agent)
{
    char *wire = msys_x11_policy_layout_wire(agent->display);
    char profile[16];
    char orientation_policy[16];
    char insets_policy[64];
    char orientation[16];
    char navigation[16];
    int width;
    int height;
    int top;
    int right;
    int bottom;
    int left;
    int x;
    int y;
    int work_width;
    int work_height;
    int nav_x;
    int nav_y;
    int nav_width;
    int nav_height;
    char *result;

    if (!wire)
        return strdup("{\"ok\":false,\"reason\":\"layout-unavailable\"}");
    if (sscanf(wire,
                "msys.layout.effective.v1;profile=%15[^;];"
                "orientation_policy=%15[^;];insets_policy=%63[^;];"
                "orientation=%15[^;];screen=%d,%d;insets=%d,%d,%d,%d;"
                "workarea=%d,%d,%d,%d;navigation=%15[^;];"
                "navigation_region=%d,%d,%d,%d",
                profile, orientation_policy, insets_policy, orientation,
                &width, &height, &top, &right, &bottom, &left,
                &x, &y, &work_width, &work_height, navigation,
                &nav_x, &nav_y, &nav_width, &nav_height) != 19) {
        free(wire);
        return strdup("{\"ok\":false,\"reason\":\"invalid-layout-state\"}");
    }
    pthread_mutex_lock(&agent->session_lock);
    result = format_json(
            "{\"ok\":true,\"schema\":\"msys.layout.effective.v1\","
            "\"profile\":\"%s\",\"orientation_policy\":\"%s\","
            "\"insets_policy\":\"%s\",\"orientation\":\"%s\","
            "\"screen\":{\"width\":%d,\"height\":%d},"
            "\"insets\":{\"top\":%d,\"right\":%d,\"bottom\":%d,\"left\":%d},"
            "\"workarea\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d},"
            "\"navigation_edge\":\"%s\","
            "\"navigation_region\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d},"
            "\"display_session\":%s,\"display_consistent\":%s}",
            profile, orientation_policy, insets_policy, orientation,
            width, height, top, right, bottom, left,
            x, y, work_width, work_height, navigation,
            nav_x, nav_y, nav_width, nav_height,
            agent->session.json ? agent->session.json :
                "{\"state\":\"unavailable\"}",
            agent->session.json && agent->session.width == width &&
                agent->session.height == height ? "true" : "false");
    pthread_mutex_unlock(&agent->session_lock);
    free(wire);
    return result;
}

static char *handle_set_layout(struct msys_x11_agent *agent,
        const char *payload)
{
    char profile[16];
    char orientation[16] = "auto";
    char insets[64] = "auto";
    char optional_orientation[16];
    char optional_insets[64];
    char *insets_object;

    if (msys_mipc_json_get_string(payload, "profile", profile,
                sizeof(profile), NULL) != MSYS_MIPC_OK &&
            msys_mipc_json_get_string(payload, "mode", profile,
                sizeof(profile), NULL) != MSYS_MIPC_OK)
        return strdup("{\"ok\":false,\"reason\":\"invalid-layout\"}");
    if (msys_mipc_json_get_string(payload, "orientation",
                optional_orientation, sizeof(optional_orientation), NULL) ==
            MSYS_MIPC_OK)
        memcpy(orientation, optional_orientation,
                strlen(optional_orientation) + 1u);
    if (msys_mipc_json_get_string(payload, "insets", optional_insets,
                sizeof(optional_insets), NULL) == MSYS_MIPC_OK) {
        memcpy(insets, optional_insets, strlen(optional_insets) + 1u);
    } else {
        int top;
        int right;
        int bottom;
        int left;

        insets_object = copy_raw(payload, "insets");
        if (insets_object &&
                json_get_int(insets_object, "top", 0, INT_MAX, &top) &&
                json_get_int(insets_object, "right", 0, INT_MAX, &right) &&
                json_get_int(insets_object, "bottom", 0, INT_MAX, &bottom) &&
                json_get_int(insets_object, "left", 0, INT_MAX, &left))
            snprintf(insets, sizeof(insets), "%d,%d,%d,%d",
                    top, right, bottom, left);
        free(insets_object);
    }
    if (msys_x11_policy_set_layout(agent->display, profile, orientation,
                insets) != 0)
        return strdup("{\"ok\":false,\"reason\":\"invalid-layout\"}");
    {
        int attempt;

        for (attempt = 0; attempt < 50; attempt++) {
            char actual_profile[16];
            char actual_orientation[16];
            char actual_insets[64];
            char *actual = layout_json(agent);

            if (actual &&
                    msys_mipc_json_get_string(actual, "profile",
                        actual_profile, sizeof(actual_profile), NULL) ==
                        MSYS_MIPC_OK &&
                    msys_mipc_json_get_string(actual, "orientation_policy",
                        actual_orientation, sizeof(actual_orientation), NULL) ==
                        MSYS_MIPC_OK &&
                    msys_mipc_json_get_string(actual, "insets_policy",
                        actual_insets, sizeof(actual_insets), NULL) ==
                        MSYS_MIPC_OK &&
                    strcmp(actual_profile, profile) == 0 &&
                    strcmp(actual_orientation, orientation) == 0 &&
                    strcmp(actual_insets, insets) == 0)
                return actual;
            free(actual);
            {
                struct timespec delay = {0, 20 * 1000 * 1000};

                nanosleep(&delay, NULL);
            }
        }
    }
    return strdup(
            "{\"ok\":false,\"reason\":\"layout-update-not-observed\"}");
}

static char *activate_component(struct msys_x11_agent *agent,
        const char *payload)
{
    char identity[257] = "";
    char title[257] = "";
    char component[257] = "";
    char *quoted_identity;
    char *quoted_title;
    char *quoted_component;
    char *result;
    int attempt;
    int rc = 3;

    (void)msys_mipc_json_get_string(payload, "identity", identity,
            sizeof(identity), NULL);
    (void)msys_mipc_json_get_string(payload, "title", title,
            sizeof(title), NULL);
    (void)msys_mipc_json_get_string(payload, "component", component,
            sizeof(component), NULL);
    if (!*identity && !*title)
        return strdup("{\"ok\":false,\"reason\":\"missing-window-identity-and-title\"}");
    for (attempt = 0; attempt < 30; attempt++) {
        struct timespec delay = {0, 50 * 1000 * 1000};

        rc = msys_x11_policy_activate(agent->display, identity, title);
        if (rc == 0)
            break;
        nanosleep(&delay, NULL);
    }
    quoted_identity = json_quote(identity);
    quoted_title = json_quote(title);
    quoted_component = json_quote(component);
    result = quoted_identity && quoted_title && quoted_component
        ? format_json(
            "{\"ok\":%s,\"component\":%s,\"identity\":%s,"
            "\"title\":%s,\"returncode\":%d}",
            rc == 0 ? "true" : "false", quoted_component,
            quoted_identity, quoted_title, rc)
        : NULL;
    free(quoted_identity);
    free(quoted_title);
    free(quoted_component);
    return result;
}

static char *role_action(struct msys_x11_agent *agent, const char *role,
        const char *method, int timeout_ms)
{
    char target[96];
    char *payload;
    char *quoted_role;
    char *result;

    snprintf(target, sizeof(target), "role:%s", role);
    payload = public_payload(public_call(agent, target, method, "{}",
                timeout_ms));
    if (!payload)
        return strdup("{\"ok\":false,\"reason\":\"role-provider-unavailable\"}");
    quoted_role = json_quote(role);
    result = quoted_role
        ? format_json("{\"ok\":true,\"role\":%s,\"result\":%s}",
            quoted_role, payload)
        : NULL;
    free(quoted_role);
    free(payload);
    return result;
}

static char *home_action(struct msys_x11_agent *agent)
{
    char *snapshot;
    char *payload;
    char *result;
    int ok = 1;

    /* Capture the foreground task while it is still unobscured.  The X11
     * policy deliberately has no compositor dependency, so once the launcher
     * is raised XGetImage cannot reliably recover pixels from the window
     * below it.  The JSON result itself is not needed here. */
    snapshot = msys_x11_policy_list_windows_json(agent->display);
    free(snapshot);
    payload = public_payload(public_call(agent, "msys.core",
            "activate_role", "{\"role\":\"launcher\"}", 8000));

    if (!payload)
        return strdup("{\"ok\":false,\"reason\":\"launcher-activation-failed\"}");
    (void)json_get_boolean(payload, "ok", &ok);
    result = format_json(
            "{\"ok\":%s,\"role\":\"launcher\",\"role_activation\":%s}",
            ok ? "true" : "false", payload);
    free(payload);
    return result;
}

static const char *overlay_method(const char *role)
{
    return strcmp(role, "chooser") == 0 ? "cancel_choice" : "hide";
}

static int is_dismissible_role(const char *role)
{
    return strcmp(role, "screen-shield") == 0 ||
        strcmp(role, "chooser") == 0 ||
        strcmp(role, "notification-center") == 0 ||
        strcmp(role, "task-switcher") == 0 ||
        strcmp(role, "input-method") == 0;
}

/* 1 = handled in-app, 0 = app requested lifecycle fallback, -1 = fail closed. */
static int application_back(struct msys_x11_agent *agent, char **detail)
{
    char *payload;
    int handled = 0;
    int fallback = 0;

    *detail = NULL;
    payload = public_payload(public_call(agent, "msys.core",
            "navigation_back", "{}", 1500));
    if (!payload)
        return -1;
    *detail = payload;
    if (!json_get_boolean(payload, "handled", &handled) ||
            !json_get_boolean(payload, "fallback", &fallback))
        return -1;
    if (handled)
        return 1;
    return fallback ? 0 : -1;
}

static char *component_lifecycle_call(struct msys_x11_agent *agent,
        const char *method, const char *component, int timeout_ms)
{
    char *quoted = json_quote(component);
    char *request = quoted
        ? format_json("{\"component\":%s}", quoted) : NULL;
    char *payload = request ? public_payload(public_call(agent,
            "msys.core", method, request, timeout_ms)) : NULL;

    free(quoted);
    free(request);
    return payload;
}

static char *back_action(struct msys_x11_agent *agent, int navigate_app)
{
    struct msys_x11_window_summary window = {0};
    char component[257];
    char next_component[257];
    char *foreground;
    char *response;
    char *payload;
    char *quoted;
    char *result;
    const char *dismiss_payload;

    if (msys_x11_policy_top_window(agent->display, 1, &window) == 1 &&
            is_dismissible_role(window.role)) {
        dismiss_payload = strcmp(window.role, "input-method") == 0
            ? "{\"reason\":\"navigation-back\",\"restore_target\":false}"
            : "{}";
        payload = public_payload(public_call(agent,
                strcmp(window.role, "chooser") == 0 ? "role:chooser" :
                strcmp(window.role, "notification-center") == 0 ?
                    "role:notification-center" :
                strcmp(window.role, "task-switcher") == 0 ?
                    "role:task-switcher" :
                strcmp(window.role, "input-method") == 0 ?
                    "role:input-method" : "role:screen-shield",
                overlay_method(window.role), dismiss_payload, 3000));
        if (!payload)
            return strdup("{\"ok\":false,\"reason\":\"overlay-dismiss-failed\"}");
        quoted = json_quote(window.role);
        result = quoted ? format_json(
                "{\"ok\":true,\"dismissed\":%s,\"overlay\":%s}",
                quoted, payload) : NULL;
        free(quoted);
        free(payload);
        return result;
    }

    (void)msys_x11_policy_top_window(agent->display, 0, &window);
    foreground = foreground_component(agent, component, sizeof(component));
    free(foreground);
    if (msys_x11_agent_home_visible(component, window.role))
        return strdup("{\"ok\":false,\"reason\":\"home-visible\"}");
    if (*component && navigate_app) {
        char *navigation = NULL;
        int navigation_result = application_back(agent, &navigation);

        if (navigation_result > 0) {
            result = navigation ? format_json(
                    "{\"ok\":true,\"destination\":\"application\","
                    "\"application_navigation\":%s}", navigation) : NULL;
            free(navigation);
            return result;
        }
        if (navigation_result < 0) {
            result = navigation ? format_json(
                    "{\"ok\":false,\"reason\":\"application-navigation-failed\","
                    "\"application_navigation\":%s}", navigation) :
                strdup("{\"ok\":false,\"reason\":\"application-navigation-failed\"}");
            free(navigation);
            return result;
        }
        free(navigation);
        /* The app may have changed foreground while handling the request.
         * Refresh before applying the root-page lifecycle fallback. */
        foreground = foreground_component(agent, component,
                sizeof(component));
        free(foreground);
        (void)msys_x11_policy_component_window(agent->display, component,
                &window);
    }
    if (*component && navigate_app) {
        char *snapshot;
        char *background;
        char *home;
        char *rollback;
        char *quoted_component;
        int home_ok = 0;
        int action_rc;

        if (strcmp(window.kind, "application") != 0 ||
                strcmp(window.component, component) != 0 ||
                !*window.window_id)
            return strdup("{\"ok\":false,\"reason\":\"managed-window-unavailable\"}");

        /* Save a clean recents thumbnail while the task is still mapped and
         * unobscured.  Backgrounding is a lifecycle/state operation; the X11
         * minimize below is intentionally separate and rollbackable. */
        snapshot = msys_x11_policy_list_windows_json(agent->display);
        free(snapshot);
        background = component_lifecycle_call(agent, "background_component",
                component, 4000);
        if (!background)
            return strdup("{\"ok\":false,\"reason\":\"component-background-failed\"}");

        action_rc = msys_x11_policy_window_action(agent->display, "minimize",
                window.window_id, 0, 0, 0, 0);
        if (action_rc != 0) {
            rollback = component_lifecycle_call(agent, "start", component,
                    8000);
            quoted_component = json_quote(component);
            result = quoted_component ? format_json(
                    "{\"ok\":false,\"reason\":\"window-minimize-failed\","
                    "\"component\":%s,\"returncode\":%d,"
                    "\"background\":%s,\"rollback\":%s}",
                    quoted_component, action_rc, background,
                    rollback ? rollback : "{\"ok\":false}") : NULL;
            free(quoted_component);
            free(background);
            free(rollback);
            return result;
        }

        home = home_action(agent);
        if (home)
            (void)json_get_boolean(home, "ok", &home_ok);
        if (!home_ok) {
            rollback = component_lifecycle_call(agent, "start", component,
                    8000);
            quoted_component = json_quote(component);
            result = quoted_component ? format_json(
                    "{\"ok\":false,\"reason\":\"home-activation-failed\","
                    "\"component\":%s,\"background\":%s,\"home\":%s,"
                    "\"rollback\":%s}", quoted_component, background,
                    home ? home : "{\"ok\":false}",
                    rollback ? rollback : "{\"ok\":false}") : NULL;
            free(quoted_component);
            free(background);
            free(home);
            free(rollback);
            return result;
        }

        quoted_component = json_quote(component);
        quoted = json_quote(window.window_id);
        result = quoted_component && quoted ? format_json(
                "{\"ok\":true,\"destination\":\"home\","
                "\"backgrounded_component\":%s,\"window_id\":%s,"
                "\"background\":%s,\"home\":%s}",
                quoted_component, quoted, background, home) : NULL;
        free(quoted_component);
        free(quoted);
        free(background);
        free(home);
        return result;
    }
    if (*component) {
        char *quoted_component = json_quote(component);
        char *stop_request = quoted_component
            ? format_json("{\"component\":%s}", quoted_component) : NULL;
        free(quoted_component);
        response = stop_request ? public_call(agent, "msys.core", "stop",
                stop_request, 4000) : NULL;
        free(stop_request);
        payload = public_payload(response);
        if (!payload)
            return strdup("{\"ok\":false,\"reason\":\"component-stop-failed\"}");
        free(payload);
        foreground = foreground_component(agent, next_component,
                sizeof(next_component));
        free(foreground);
        if (*next_component) {
            char *quoted_next = json_quote(next_component);
            char *start_request = quoted_next
                ? format_json("{\"component\":%s}", quoted_next) : NULL;
            free(quoted_next);
            payload = start_request ? public_payload(public_call(agent,
                    "msys.core", "start", start_request, 8000)) : NULL;
            free(start_request);
            if (!payload)
                return strdup("{\"ok\":false,\"reason\":\"component-restore-failed\"}");
            quoted = json_quote(next_component);
            result = quoted ? format_json(
                    "{\"ok\":true,\"destination\":\"component\","
                    "\"restored_component\":%s,\"restoration\":%s}",
                    quoted, payload) : NULL;
            free(quoted);
            free(payload);
            return result;
        }
        result = home_action(agent);
        if (result) {
            char *closed = json_quote(component);
            char *wrapped = closed ? format_json(
                    "{\"ok\":true,\"destination\":\"home\","
                    "\"closed_component\":%s,\"home\":%s}", closed,
                    result) : NULL;
            free(closed);
            free(result);
            return wrapped;
        }
        return NULL;
    }

    if (msys_x11_policy_top_window(agent->display, 0, &window) != 1 ||
            !*window.window_id)
        return strdup("{\"ok\":false,\"reason\":\"no-user-window\"}");
    if (navigate_app) {
        char *snapshot = msys_x11_policy_list_windows_json(agent->display);
        char *home;
        int home_ok = 0;

        free(snapshot);
        if (msys_x11_policy_window_action(agent->display, "minimize",
                    window.window_id, 0, 0, 0, 0) != 0)
            return strdup("{\"ok\":false,\"reason\":\"x11-action-failed\"}");
        home = home_action(agent);
        if (home)
            (void)json_get_boolean(home, "ok", &home_ok);
        if (!home_ok) {
            (void)msys_x11_policy_window_action(agent->display, "focus",
                    window.window_id, 0, 0, 0, 0);
            result = home ? format_json(
                    "{\"ok\":false,\"reason\":\"home-activation-failed\","
                    "\"home\":%s}", home) : NULL;
            free(home);
            return result;
        }
        quoted = json_quote(window.window_id);
        result = quoted ? format_json(
                "{\"ok\":true,\"destination\":\"home\","
                "\"backgrounded_window\":%s,\"home\":%s}",
                quoted, home) : NULL;
        free(quoted);
        free(home);
        return result;
    }
    if (msys_x11_policy_window_action(agent->display, "close",
                window.window_id, 0, 0, 0, 0) != 0)
        return strdup("{\"ok\":false,\"reason\":\"x11-action-failed\"}");
    quoted = json_quote(window.window_id);
    result = quoted ? format_json(
            "{\"ok\":true,\"schema\":\"msys.window-action.v1\","
            "\"action\":\"close\",\"window_id\":%s}", quoted) : NULL;
    free(quoted);
    return result;
}

static char *window_action_call(struct msys_x11_agent *agent,
        const char *method, const char *payload)
{
    char action[32] = "";
    char window_id[MSYS_X11_SUMMARY_ID_MAX] = "";
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int rc;
    char *quoted;
    char *result;

    if (strcmp(method, "window_action") == 0)
        (void)msys_mipc_json_get_string(payload, "action", action,
                sizeof(action), NULL);
    else if (strcmp(method, "focus_window") == 0)
        memcpy(action, "focus", sizeof("focus"));
    else if (strcmp(method, "minimize_window") == 0)
        memcpy(action, "minimize", sizeof("minimize"));
    else if (strcmp(method, "move_window") == 0)
        memcpy(action, "move", sizeof("move"));
    else if (strcmp(method, "resize_window") == 0)
        memcpy(action, "resize", sizeof("resize"));
    else if (strcmp(method, "move_resize_window") == 0)
        memcpy(action, "move-resize", sizeof("move-resize"));
    else if (strcmp(method, "maximize_window") == 0)
        memcpy(action, "maximize", sizeof("maximize"));
    else if (strcmp(method, "restore_window") == 0)
        memcpy(action, "restore", sizeof("restore"));
    else if (strcmp(method, "snap_left_window") == 0)
        memcpy(action, "snap-left", sizeof("snap-left"));
    else if (strcmp(method, "snap_right_window") == 0)
        memcpy(action, "snap-right", sizeof("snap-right"));
    else if (strcmp(method, "close_window") == 0)
        memcpy(action, "close", sizeof("close"));
    else
        return NULL;
    if (strcmp(action, "move_resize") == 0)
        memcpy(action, "move-resize", sizeof("move-resize"));
    if (msys_mipc_json_get_string(payload, "window_id", window_id,
                sizeof(window_id), NULL) != MSYS_MIPC_OK)
        (void)msys_mipc_json_get_string(payload, "id", window_id,
                sizeof(window_id), NULL);
    if (!*window_id)
        return strdup("{\"ok\":false,\"reason\":\"invalid-window-id\"}");
    if (strcmp(action, "close") == 0) {
        struct msys_x11_window_summary summary;

        if (msys_x11_policy_window_summary(agent->display, window_id,
                    &summary) == 1 && *summary.component &&
                strcmp(summary.kind, "application") == 0) {
            char active[257];
            char *foreground = foreground_component(agent, active,
                    sizeof(active));
            int managed = foreground && foreground_contains_component(
                    foreground, summary.component);
            free(foreground);
            if (managed) {
                char *quoted_component = json_quote(summary.component);
                char *stop_request = quoted_component ? format_json(
                        "{\"component\":%s}", quoted_component) : NULL;
                char *stop_result = stop_request ? public_payload(public_call(
                        agent, "msys.core", "stop", stop_request, 4000)) : NULL;
                free(quoted_component);
                free(stop_request);
                if (!stop_result)
                    return strdup("{\"ok\":false,\"schema\":\"msys.window-action.v1\",\"action\":\"close\",\"reason\":\"component-stop-failed\"}");
                free(stop_result);
                quoted = json_quote(window_id);
                quoted_component = json_quote(summary.component);
                result = quoted && quoted_component ? format_json(
                        "{\"ok\":true,\"schema\":\"msys.window-action.v1\","
                        "\"action\":\"close\",\"window_id\":%s,"
                        "\"closed_component\":%s,\"returncode\":0}",
                        quoted, quoted_component) : NULL;
                free(quoted);
                free(quoted_component);
                return result;
            }
        }
    }
    if (strcmp(action, "move") == 0 || strcmp(action, "move-resize") == 0) {
        if (!json_get_int(payload, "x", -32768, 32767, &x) ||
                !json_get_int(payload, "y", -32768, 32767, &y))
            return strdup("{\"ok\":false,\"reason\":\"invalid-geometry\"}");
    }
    if (strcmp(action, "resize") == 0 ||
            strcmp(action, "move-resize") == 0) {
        if (!json_get_int(payload, "width", 1, 32767, &width) ||
                !json_get_int(payload, "height", 1, 32767, &height))
            return strdup("{\"ok\":false,\"reason\":\"invalid-geometry\"}");
    }
    {
        struct msys_x11_window_action_result outcome;
        char *quoted_action;
        char *quoted_profile;
        char *quoted_placement;
        char *quoted_state;
        char *quoted_reason;
        char geometry[160];
        char restore_geometry[160];

        rc = msys_x11_policy_window_action_result(agent->display, action,
                window_id, x, y, width, height, &outcome);
        quoted = json_quote(window_id);
        quoted_action = json_quote(outcome.action);
        quoted_profile = json_quote(outcome.profile);
        quoted_placement = json_quote(outcome.placement);
        quoted_state = json_quote(outcome.state);
        quoted_reason = json_quote(outcome.reason);
        if (outcome.has_geometry)
            snprintf(geometry, sizeof(geometry),
                    "{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
                    outcome.x, outcome.y, outcome.width, outcome.height);
        else
            memcpy(geometry, "null", sizeof("null"));
        if (outcome.has_restore_geometry)
            snprintf(restore_geometry, sizeof(restore_geometry),
                    "{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
                    outcome.restore_x, outcome.restore_y,
                    outcome.restore_width, outcome.restore_height);
        else
            memcpy(restore_geometry, "null", sizeof("null"));
        result = quoted && quoted_action && quoted_profile &&
                quoted_placement && quoted_state && quoted_reason
            ? format_json(
                "{\"ok\":%s,\"schema\":\"msys.window-action.v1\","
                "\"action\":%s,\"window_id\":%s,\"returncode\":%d,"
                "\"profile\":%s,\"placement\":%s,\"state\":%s,"
                "\"geometry\":%s,\"restore_geometry\":%s%s%s%s}",
                rc == 0 ? "true" : "false", quoted_action, quoted, rc,
                quoted_profile, quoted_placement, quoted_state, geometry,
                restore_geometry, *outcome.reason ? ",\"reason\":" : "",
                *outcome.reason ? quoted_reason : "",
                *outcome.reason ? "" : "") : NULL;
        free(quoted_action);
        free(quoted_profile);
        free(quoted_placement);
        free(quoted_state);
        free(quoted_reason);
    }
    free(quoted);
    return result;
}

static char *handle_call(struct msys_x11_agent *agent, const char *method,
        const char *payload)
{
    char *result;

    if (strcmp(method, "list_windows") == 0 ||
            strcmp(method, "recents") == 0)
        return msys_x11_policy_list_windows_json(agent->display);
    if (strcmp(method, "get_layout") == 0 || strcmp(method, "layout") == 0)
        return layout_json(agent);
    if (strcmp(method, "set_layout") == 0)
        return handle_set_layout(agent, payload);
    if (strcmp(method, "get_display_session") == 0 ||
            strcmp(method, "display_session") == 0) {
        pthread_mutex_lock(&agent->session_lock);
        result = agent->session.json
            ? format_json("{\"ok\":true,\"display_session\":%s}",
                agent->session.json)
            : strdup("{\"ok\":false,\"reason\":\"display-session-unavailable\"}");
        pthread_mutex_unlock(&agent->session_lock);
        return result;
    }
    if (strcmp(method, "activate_component") == 0)
        return activate_component(agent, payload);
    if (strcmp(method, "home") == 0)
        return home_action(agent);
    if (strcmp(method, "back") == 0)
        return back_action(agent, 1);
    if (strcmp(method, "close_active") == 0)
        return back_action(agent, 0);
    if (strcmp(method, "navigation_action") == 0 ||
            strcmp(method, "navigate") == 0) {
        char action[32] = "";
        char input[32] = "button";
        char optional_input[32];
        char *delegated;
        char *quoted_action;
        char *quoted_input;
        int delegated_ok = 0;

        (void)msys_mipc_json_get_string(payload, "action", action,
                sizeof(action), NULL);
        if (msys_mipc_json_get_string(payload, "input", optional_input,
                    sizeof(optional_input), NULL) == MSYS_MIPC_OK)
            memcpy(input, optional_input, strlen(optional_input) + 1u);
        if (strcmp(action, "apps") == 0 || strcmp(action, "recents") == 0) {
            memcpy(action, "recents", sizeof("recents"));
            delegated = role_action(agent, "task-switcher", "show", 7000);
        } else if (strcmp(action, "home") == 0) {
            delegated = home_action(agent);
        } else if (strcmp(action, "back") == 0) {
            delegated = back_action(agent, 1);
        } else {
            return strdup("{\"ok\":false,\"schema\":\"msys.navigation-action.v1\",\"reason\":\"invalid-navigation-action\"}");
        }
        quoted_action = json_quote(action);
        quoted_input = json_quote(input);
        if (delegated)
            (void)json_get_boolean(delegated, "ok", &delegated_ok);
        result = delegated && quoted_action && quoted_input
            ? format_json(
                "{\"schema\":\"msys.navigation-action.v1\","
                "\"action\":%s,\"input\":%s,\"result\":%s,"
                "\"ok\":%s}", quoted_action, quoted_input, delegated,
                delegated_ok ? "true" : "false")
            : NULL;
        free(delegated);
        free(quoted_action);
        free(quoted_input);
        return result;
    }
    result = window_action_call(agent, method, payload);
    if (result)
        return result;
    return strdup("{\"ok\":false,\"reason\":\"unsupported-native-method\",\"fallback\":\"org.msys.x11.session:window-policy-python\"}");
}

static char *layout_changed_event(const char *layout)
{
    char profile[16];
    char orientation_policy[16];
    char orientation[16];
    char navigation_edge[16];
    char *geometry = NULL;
    char *insets = NULL;
    char *workarea = NULL;
    char *navigation_region = NULL;
    char *result = NULL;
    int display_consistent = 0;

    if (msys_mipc_json_get_string(layout, "profile", profile,
                sizeof(profile), NULL) != MSYS_MIPC_OK ||
            msys_mipc_json_get_string(layout, "orientation_policy",
                orientation_policy, sizeof(orientation_policy), NULL) !=
                MSYS_MIPC_OK ||
            msys_mipc_json_get_string(layout, "orientation", orientation,
                sizeof(orientation), NULL) != MSYS_MIPC_OK ||
            msys_mipc_json_get_string(layout, "navigation_edge",
                navigation_edge, sizeof(navigation_edge), NULL) !=
                MSYS_MIPC_OK ||
            !json_get_boolean(layout, "display_consistent",
                &display_consistent))
        goto done;
    geometry = copy_raw(layout, "screen");
    insets = copy_raw(layout, "insets");
    workarea = copy_raw(layout, "workarea");
    navigation_region = copy_raw(layout, "navigation_region");
    if (!geometry || !insets || !workarea || !navigation_region)
        goto done;
    result = format_json(
            "{\"schema\":\"msys.layout.changed.v1\","
            "\"profile\":\"%s\",\"orientation_policy\":\"%s\","
            "\"orientation\":\"%s\",\"geometry\":%s,"
            "\"insets\":%s,\"workarea\":%s,"
            "\"navigation_edge\":\"%s\",\"navigation_region\":%s,"
            "\"display_consistent\":%s}",
            profile, orientation_policy, orientation, geometry, insets,
            workarea, navigation_edge, navigation_region,
            display_consistent ? "true" : "false");

done:
    free(geometry);
    free(insets);
    free(workarea);
    free(navigation_region);
    return result;
}

static void *worker_main(void *opaque)
{
    struct worker_call *call = opaque;
    struct msys_x11_agent *agent = call->agent;
    char *result = handle_call(agent, call->method, call->payload);

    if (!result)
        result = strdup("{\"ok\":false,\"reason\":\"native-agent-out-of-memory\"}");
    if (result)
        (void)send_return_locked(agent, call->id, result);
    if (result && strcmp(call->method, "set_layout") == 0 &&
            strstr(result, "\"ok\":true") != NULL) {
        char *event = layout_changed_event(result);
        if (event) {
            (void)send_event_locked(agent, "msys.layout.changed", event);
            free(event);
        }
    }
    if (result && strstr(result,
                "\"schema\":\"msys.window-action.v1\"") != NULL)
        (void)send_event_locked(agent, "msys.window.action", result);
    free(result);
    free(call->payload);
    free(call);
    pthread_mutex_lock(&agent->worker_lock);
    if (agent->workers > 0)
        agent->workers--;
    if (agent->workers == 0)
        pthread_cond_broadcast(&agent->worker_idle);
    pthread_mutex_unlock(&agent->worker_lock);
    return NULL;
}

static int dispatch_call(struct msys_x11_agent *agent, const char *packet)
{
    struct worker_call *call = calloc(1, sizeof(*call));
    pthread_attr_t attributes;
    pthread_t thread;
    int result;

    if (!call)
        return 0;
    call->agent = agent;
    if (msys_mipc_json_get_u64(packet, "id", &call->id) != MSYS_MIPC_OK ||
            msys_mipc_json_get_string(packet, "method", call->method,
                sizeof(call->method), NULL) != MSYS_MIPC_OK) {
        free(call);
        return 0;
    }
    call->payload = copy_raw(packet, "payload");
    if (!call->payload)
        call->payload = strdup("{}");
    if (!call->payload) {
        free(call);
        return 0;
    }
    pthread_mutex_lock(&agent->worker_lock);
    if (agent->stopping) {
        pthread_mutex_unlock(&agent->worker_lock);
        free(call->payload);
        free(call);
        return 0;
    }
    agent->workers++;
    pthread_mutex_unlock(&agent->worker_lock);
    pthread_attr_init(&attributes);
    (void)pthread_attr_setstacksize(&attributes, AGENT_WORKER_STACK);
    result = pthread_create(&thread, &attributes, worker_main, call);
    pthread_attr_destroy(&attributes);
    if (result == 0) {
        pthread_detach(thread);
        return 1;
    }
    pthread_mutex_lock(&agent->worker_lock);
    agent->workers--;
    pthread_mutex_unlock(&agent->worker_lock);
    free(call->payload);
    free(call);
    return 0;
}

static char *read_file_bounded(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    char *buffer;
    size_t length = 0;

    if (fd < 0)
        return NULL;
    buffer = malloc(MSYS_MIPC_RECV_CAPACITY);
    if (!buffer) {
        close(fd);
        return NULL;
    }
    while (length < MSYS_MIPC_MAX_PACKET) {
        ssize_t count = read(fd, buffer + length,
                MSYS_MIPC_MAX_PACKET - length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            free(buffer);
            close(fd);
            return NULL;
        }
        if (count == 0)
            break;
        length += (size_t)count;
    }
    close(fd);
    if (length == MSYS_MIPC_MAX_PACKET) {
        free(buffer);
        return NULL;
    }
    buffer[length] = '\0';
    return buffer;
}

static int matrix_array_to_wire(const char *json, char *output, size_t size)
{
    const char *cursor = json;
    size_t used = 0;
    int field;

    if (!cursor || *cursor++ != '[')
        return 0;
    for (field = 0; field < 9; field++) {
        char *end = NULL;
        double value;
        int written;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
                *cursor == '\n')
            cursor++;
        errno = 0;
        value = strtod(cursor, &end);
        if (errno || !end || end == cursor)
            return 0;
        written = snprintf(output + used, size - used, field ? ",%.12g" :
                "%.12g", value);
        if (written < 0 || (size_t)written >= size - used)
            return 0;
        used += (size_t)written;
        cursor = end;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
                *cursor == '\n')
            cursor++;
        if (field < 8) {
            if (*cursor++ != ',')
                return 0;
        } else if (*cursor++ != ']') {
            return 0;
        }
    }
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
            *cursor == '\n')
        cursor++;
    return *cursor == '\0';
}

static int parse_display_session(char *json, struct display_session *session)
{
    char schema[64];
    char state[32];
    char *geometry = NULL;
    char *transform = NULL;
    char *matrix = NULL;
    uint64_t width;
    uint64_t height;
    uint64_t depth;
    uint64_t observed;
    char space[64];
    char mode[64];
    char source[96];
    int verified;
    int result = 0;

    memset(session, 0, sizeof(*session));
    if (msys_mipc_json_get_string(json, "schema", schema, sizeof(schema),
                NULL) != MSYS_MIPC_OK ||
            strcmp(schema, "msys.display-session.v1") != 0 ||
            msys_mipc_json_get_string(json, "state", state, sizeof(state),
                NULL) != MSYS_MIPC_OK || strcmp(state, "ready") != 0 ||
            msys_mipc_json_get_string(json, "provider", session->provider,
                sizeof(session->provider), NULL) != MSYS_MIPC_OK ||
            !valid_component_name(session->provider) ||
            msys_mipc_json_get_string(json, "display", session->display,
                sizeof(session->display), NULL) != MSYS_MIPC_OK ||
            !valid_display_name(session->display) ||
            msys_mipc_json_get_u64(json, "generation", &session->generation) !=
                MSYS_MIPC_OK ||
            msys_mipc_json_get_u64(json, "observed_at_unix_ms", &observed) !=
                MSYS_MIPC_OK)
        goto done;
    (void)observed;
    geometry = copy_raw(json, "geometry");
    transform = copy_raw(json, "input_transform");
    if (!geometry || !transform ||
            msys_mipc_json_get_u64(geometry, "width", &width) != MSYS_MIPC_OK ||
            msys_mipc_json_get_u64(geometry, "height", &height) != MSYS_MIPC_OK ||
            msys_mipc_json_get_u64(geometry, "depth", &depth) != MSYS_MIPC_OK ||
            width < 1 || width > 65535 || height < 1 || height > 65535 ||
            depth > 128 || !json_get_boolean(transform, "enabled",
                &session->input_enabled) ||
            msys_mipc_json_get_string(transform, "space", space,
                sizeof(space), NULL) != MSYS_MIPC_OK ||
            strcmp(space, "normalized-display") != 0 ||
            msys_mipc_json_get_string(transform, "mode", mode,
                sizeof(mode), NULL) != MSYS_MIPC_OK || !*mode ||
            msys_mipc_json_get_string(transform, "source", source,
                sizeof(source), NULL) != MSYS_MIPC_OK || !*source ||
            !json_get_boolean(transform, "verified", &verified) || !verified)
        goto done;
    session->width = (int)width;
    session->height = (int)height;
    session->depth = (int)depth;
    matrix = copy_raw(transform, "matrix");
    if (session->input_enabled) {
        if (!matrix || !matrix_array_to_wire(matrix, session->matrix,
                    sizeof(session->matrix)))
            goto done;
    } else {
        if (!matrix || strcmp(matrix, "null") != 0)
            goto done;
        memcpy(session->matrix, "none", sizeof("none"));
    }
    session->json = json;
    result = 1;

done:
    free(geometry);
    free(transform);
    free(matrix);
    return result;
}

static int refresh_display_session(struct msys_x11_agent *agent, int force,
        int emit_event)
{
    struct stat status;
    struct display_session session;
    char *json;
    char *event;

    if (stat(agent->state_path, &status) != 0) {
        agent->have_state_stat = 0;
        return 0;
    }
    if (!force && agent->have_state_stat &&
            status.st_dev == agent->state_stat.st_dev &&
            status.st_ino == agent->state_stat.st_ino &&
            status.st_mtime == agent->state_stat.st_mtime &&
            status.st_size == agent->state_stat.st_size)
        return 0;
    json = read_file_bounded(agent->state_path);
    if (!json || !parse_display_session(json, &session)) {
        free(json);
        return 0;
    }
    if (*agent->display && strcmp(agent->display, session.display) != 0) {
        free(json);
        return 75;
    }
    if (msys_x11_policy_sync_display(agent->display, session.width,
                session.height, session.depth, session.input_enabled,
                session.matrix) != 0) {
        free(json);
        return 0;
    }
    pthread_mutex_lock(&agent->session_lock);
    free(agent->session.json);
    agent->session = session;
    pthread_mutex_unlock(&agent->session_lock);
    agent->state_stat = status;
    agent->have_state_stat = 1;
    event = format_json(
            "{\"provider\":\"%s\",\"generation\":%llu,"
            "\"display\":\"%s\",\"geometry\":{\"width\":%d,"
            "\"height\":%d,\"depth\":%d}}",
            session.provider, (unsigned long long)session.generation,
            session.display, session.width, session.height, session.depth);
    if (emit_event && event) {
        (void)send_event_locked(agent, "msys.display_session.applied", event);
    }
    free(event);
    return 0;
}

int msys_x11_agent_start(struct msys_x11_agent **output,
        const char *display_name)
{
    struct msys_x11_agent *agent;
    const char *control_fd = getenv("MSYS_CONTROL_FD");
    const char *native_enabled = getenv("MSYS_X11_NATIVE_AGENT");
    const char *runtime;
    const char *state_path;
    char *packet;
    int result;

    if (!output)
        return 64;
    *output = NULL;
    if (native_enabled && strcmp(native_enabled, "0") == 0)
        return 0;
    if (!control_fd || !*control_fd)
        return 0;
    agent = calloc(1, sizeof(*agent));
    if (!agent)
        return 1;
    agent->recv_packet = malloc(MSYS_MIPC_RECV_CAPACITY);
    if (!agent->recv_packet) {
        free(agent);
        return 1;
    }
    pthread_mutex_init(&agent->send_lock, NULL);
    pthread_mutex_init(&agent->worker_lock, NULL);
    pthread_mutex_init(&agent->session_lock, NULL);
    pthread_cond_init(&agent->worker_idle, NULL);
    snprintf(agent->display, sizeof(agent->display), "%s",
            display_name ? display_name : "");
    runtime = getenv("MSYS_RUNTIME_DIR");
    if (!runtime || !*runtime)
        runtime = "/run/msys/main";
    snprintf(agent->runtime_dir, sizeof(agent->runtime_dir), "%s", runtime);
    state_path = getenv("MSYS_DISPLAY_SESSION_STATE_FILE");
    if (state_path && *state_path)
        snprintf(agent->state_path, sizeof(agent->state_path), "%s",
                state_path);
    else
        snprintf(agent->state_path, sizeof(agent->state_path),
                "%s/display-session.json", runtime);
    result = msys_mipc_client_from_env(&agent->client);
    if (result != MSYS_MIPC_OK)
        goto failed;
    result = msys_mipc_send_hello_from_env(&agent->client);
    if (result != MSYS_MIPC_OK)
        goto failed;
    packet = agent->recv_packet;
    result = msys_mipc_recv_json(&agent->client, packet,
            MSYS_MIPC_RECV_CAPACITY, 2000, NULL);
    if (result != MSYS_MIPC_OK || !packet_type_is(packet, "welcome")) {
        goto failed;
    }
    result = refresh_display_session(agent, 1, 0);
    if (result == 75)
        goto moved;
    result = msys_mipc_send_ready(&agent->client);
    if (result != MSYS_MIPC_OK)
        goto failed;
    {
        const char *component = getenv("MSYS_COMPONENT_ID");
        char *quoted = json_quote(component && *component ? component :
                AGENT_COMPONENT_FALLBACK);
        char *ready = quoted ? format_json("{\"component\":%s,"
                "\"implementation\":\"native-c\",\"pid\":%ld}",
                quoted, (long)getpid()) : NULL;
        free(quoted);
        if (!ready || send_event_locked(agent, "msys.window_policy.ready",
                    ready) != MSYS_MIPC_OK) {
            free(ready);
            goto failed;
        }
        free(ready);
    }
    if (agent->session.json) {
        char *applied = format_json(
                "{\"provider\":\"%s\",\"generation\":%llu,"
                "\"display\":\"%s\",\"geometry\":{\"width\":%d,"
                "\"height\":%d,\"depth\":%d}}",
                agent->session.provider,
                (unsigned long long)agent->session.generation,
                agent->session.display, agent->session.width,
                agent->session.height, agent->session.depth);
        if (applied) {
            (void)send_event_locked(agent, "msys.display_session.applied",
                    applied);
            free(applied);
        }
    }
    *output = agent;
    return 0;

moved:
    msys_mipc_client_close(&agent->client);
    free(agent->session.json);
    free(agent->recv_packet);
    pthread_cond_destroy(&agent->worker_idle);
    pthread_mutex_destroy(&agent->session_lock);
    pthread_mutex_destroy(&agent->worker_lock);
    pthread_mutex_destroy(&agent->send_lock);
    free(agent);
    return 75;

failed:
    msys_mipc_client_close(&agent->client);
    free(agent->session.json);
    free(agent->recv_packet);
    pthread_cond_destroy(&agent->worker_idle);
    pthread_mutex_destroy(&agent->session_lock);
    pthread_mutex_destroy(&agent->worker_lock);
    pthread_mutex_destroy(&agent->send_lock);
    free(agent);
    return 1;
}

int msys_x11_agent_poll(struct msys_x11_agent *agent)
{
    char *packet;
    char type[32];
    uint64_t now = 0;
    int result;

    if (!agent)
        return 0;
    packet = agent->recv_packet;
    result = msys_mipc_recv_json(&agent->client, packet,
            MSYS_MIPC_RECV_CAPACITY, 0, NULL);
    if (result == MSYS_MIPC_EOF) {
        return AGENT_SHUTDOWN;
    }
    if (result != MSYS_MIPC_OK && result != MSYS_MIPC_TIMEOUT) {
        return 1;
    }
    if (result == MSYS_MIPC_OK &&
            msys_mipc_json_get_string(packet, "type", type, sizeof(type),
                NULL) == MSYS_MIPC_OK) {
        if (strcmp(type, "shutdown") == 0) {
            return AGENT_SHUTDOWN;
        }
        if (strcmp(type, "call") == 0 && !dispatch_call(agent, packet)) {
            uint64_t id = 0;
            (void)msys_mipc_json_get_u64(packet, "id", &id);
            (void)send_return_locked(agent, id,
                    "{\"ok\":false,\"reason\":\"native-dispatch-failed\"}");
        }
    }
    if (msys_mipc_monotonic_ms(&now) == MSYS_MIPC_OK &&
            now >= agent->next_state_poll_ms) {
        agent->next_state_poll_ms = now + 200u;
        result = refresh_display_session(agent, 0, 1);
        if (result != 0)
            return result;
    }
    return 0;
}

void msys_x11_agent_stop(struct msys_x11_agent *agent)
{
    if (!agent)
        return;
    pthread_mutex_lock(&agent->worker_lock);
    agent->stopping = 1;
    while (agent->workers > 0)
        pthread_cond_wait(&agent->worker_idle, &agent->worker_lock);
    pthread_mutex_unlock(&agent->worker_lock);
    msys_mipc_client_close(&agent->client);
    free(agent->session.json);
    free(agent->recv_packet);
    pthread_cond_destroy(&agent->worker_idle);
    pthread_mutex_destroy(&agent->session_lock);
    pthread_mutex_destroy(&agent->worker_lock);
    pthread_mutex_destroy(&agent->send_lock);
    free(agent);
}
