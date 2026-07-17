#ifndef MSYS_LAYOUT_H
#define MSYS_LAYOUT_H

#include <stddef.h>

#define MSYS_LAYOUT_CONFIG_SCHEMA "msys.layout.v1"
#define MSYS_LAYOUT_EFFECTIVE_SCHEMA "msys.layout.effective.v1"
#define MSYS_LAYOUT_TEXT_MAX 384

enum msys_layout_profile {
    MSYS_LAYOUT_MOBILE = 0,
    MSYS_LAYOUT_KIOSK,
    MSYS_LAYOUT_DESKTOP
};

enum msys_orientation_policy {
    MSYS_ORIENTATION_AUTO = 0,
    MSYS_ORIENTATION_PORTRAIT,
    MSYS_ORIENTATION_LANDSCAPE
};

enum msys_orientation {
    MSYS_PORTRAIT = 0,
    MSYS_LANDSCAPE
};

enum msys_navigation_edge {
    MSYS_NAVIGATION_BOTTOM = 0,
    MSYS_NAVIGATION_RIGHT
};

enum msys_surface_kind {
    MSYS_SURFACE_APPLICATION = 0,
    MSYS_SURFACE_INPUT_METHOD,
    MSYS_SURFACE_CHROME,
    MSYS_SURFACE_NAVIGATION,
    MSYS_SURFACE_NOTIFICATION,
    MSYS_SURFACE_RECENTS,
    MSYS_SURFACE_CHOOSER,
    MSYS_SURFACE_TRANSITION,
    MSYS_SURFACE_SHIELD
};

struct msys_insets {
    int top;
    int right;
    int bottom;
    int left;
};

struct msys_rect {
    int x;
    int y;
    int width;
    int height;
};

struct msys_layout_config {
    enum msys_layout_profile profile;
    enum msys_orientation_policy orientation;
    int automatic_insets;
    struct msys_insets insets;
};

struct msys_layout_state {
    struct msys_layout_config config;
    enum msys_orientation orientation;
    enum msys_navigation_edge navigation_edge;
    int screen_width;
    int screen_height;
    int bar_size;
    int gap;
    struct msys_insets insets;
    struct msys_rect workarea;
};

void msys_layout_config_default(struct msys_layout_config *config);
int msys_layout_config_parse(struct msys_layout_config *config,
        const char *profile, const char *orientation, const char *insets);
int msys_layout_config_encode(const struct msys_layout_config *config,
        char *buffer, size_t size);
int msys_layout_config_decode(const char *text,
        struct msys_layout_config *config);
void msys_layout_resolve(const struct msys_layout_config *config,
        int screen_width, int screen_height, struct msys_layout_state *state);
void msys_layout_place(const struct msys_layout_state *state,
        enum msys_surface_kind surface, const struct msys_rect *requested,
        struct msys_rect *result);
int msys_layout_effective_encode(const struct msys_layout_state *state,
        char *buffer, size_t size);

const char *msys_layout_profile_name(enum msys_layout_profile profile);
const char *msys_orientation_policy_name(
        enum msys_orientation_policy orientation);
const char *msys_orientation_name(enum msys_orientation orientation);
const char *msys_navigation_edge_name(enum msys_navigation_edge edge);

#endif
