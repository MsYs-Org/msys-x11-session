#define _POSIX_C_SOURCE 200809L

#include "msys_x11_policy_api.h"

#include <stdlib.h>
#include <string.h>

char *msys_x11_policy_list_windows_json(const char *display_name)
{
    (void)display_name;
    return strdup("{\"schema\":\"msys.window-list.v1\",\"windows\":[]}");
}

char *msys_x11_policy_layout_wire(const char *display_name)
{
    (void)display_name;
    return strdup(
        "msys.layout.effective.v1;profile=mobile;orientation_policy=auto;"
        "insets_policy=auto;orientation=portrait;screen=320,480;"
        "insets=48,0,48,0;workarea=0,48,320,384;navigation=bottom;"
        "navigation_region=0,432,320,48");
}

int msys_x11_policy_top_window(const char *display_name,
        int dismissible_only, struct msys_x11_window_summary *summary)
{
    (void)display_name;
    (void)dismissible_only;
    memset(summary, 0, sizeof(*summary));
    return 0;
}

int msys_x11_policy_component_window(const char *display_name,
        const char *component, struct msys_x11_window_summary *summary)
{
    (void)display_name;
    (void)component;
    memset(summary, 0, sizeof(*summary));
    return 0;
}

int msys_x11_policy_window_summary(const char *display_name,
        const char *window_id, struct msys_x11_window_summary *summary)
{
    (void)display_name;
    (void)window_id;
    memset(summary, 0, sizeof(*summary));
    return 0;
}

int msys_x11_policy_window_action(const char *display_name,
        const char *action, const char *window_id,
        int x, int y, int width, int height)
{
    (void)display_name;
    (void)action;
    (void)window_id;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    return 0;
}

int msys_x11_policy_activate(const char *display_name,
        const char *identity, const char *title)
{
    (void)display_name;
    (void)identity;
    (void)title;
    return 0;
}

int msys_x11_policy_close_identity(const char *display_name,
        const char *identity, const char *title)
{
    (void)display_name;
    (void)identity;
    (void)title;
    return 0;
}

int msys_x11_policy_set_layout(const char *display_name,
        const char *profile, const char *orientation, const char *insets)
{
    (void)display_name;
    return strcmp(profile, "mobile") != 0 || strcmp(orientation, "auto") != 0 ||
        strcmp(insets, "auto") != 0;
}

int msys_x11_policy_sync_display(const char *display_name,
        int width, int height, int depth, int input_enabled,
        const char *input_matrix)
{
    (void)display_name;
    (void)width;
    (void)height;
    (void)depth;
    (void)input_enabled;
    (void)input_matrix;
    return 0;
}

int msys_x11_publish_display_session(const char *display_name,
        const char *provider, const char *state_file)
{
    (void)display_name;
    (void)provider;
    (void)state_file;
    return 0;
}
