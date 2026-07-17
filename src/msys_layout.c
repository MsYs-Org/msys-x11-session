#define _POSIX_C_SOURCE 200809L

#include "msys_layout.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static int adaptive_metric(int short_side, int divisor, int minimum,
        int maximum)
{
    int value;
    int screen_limit;

    if (short_side < 1)
        return 1;
    value = short_side / divisor;
    value = clamp_int(value, minimum, maximum);
    screen_limit = short_side / 3;
    if (screen_limit < 1)
        screen_limit = 1;
    return value > screen_limit ? screen_limit : value;
}

static int parse_profile(const char *value, enum msys_layout_profile *profile)
{
    if (value && strcmp(value, "mobile") == 0)
        *profile = MSYS_LAYOUT_MOBILE;
    else if (value && strcmp(value, "kiosk") == 0)
        *profile = MSYS_LAYOUT_KIOSK;
    else if (value && strcmp(value, "desktop") == 0)
        *profile = MSYS_LAYOUT_DESKTOP;
    else
        return 0;
    return 1;
}

static int parse_orientation(const char *value,
        enum msys_orientation_policy *orientation)
{
    if (value && strcmp(value, "auto") == 0)
        *orientation = MSYS_ORIENTATION_AUTO;
    else if (value && strcmp(value, "portrait") == 0)
        *orientation = MSYS_ORIENTATION_PORTRAIT;
    else if (value && strcmp(value, "landscape") == 0)
        *orientation = MSYS_ORIENTATION_LANDSCAPE;
    else
        return 0;
    return 1;
}

static int parse_nonnegative(const char **cursor, int *result)
{
    const char *start = *cursor;
    char *end = NULL;
    long parsed;

    if (!start || *start < '0' || *start > '9')
        return 0;
    parsed = strtol(start, &end, 10);
    if (!end || end == start || parsed < 0 || parsed > INT_MAX)
        return 0;
    *cursor = end;
    *result = (int)parsed;
    return 1;
}

static int parse_insets(const char *value, int *automatic,
        struct msys_insets *insets)
{
    const char *cursor = value;
    int *fields[] = {
        &insets->top,
        &insets->right,
        &insets->bottom,
        &insets->left
    };
    size_t i;

    memset(insets, 0, sizeof(*insets));
    if (value && strcmp(value, "auto") == 0) {
        *automatic = 1;
        return 1;
    }
    if (!value || !*value)
        return 0;
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!parse_nonnegative(&cursor, fields[i]))
            return 0;
        if (i + 1 < sizeof(fields) / sizeof(fields[0])) {
            if (*cursor != ',')
                return 0;
            cursor++;
        }
    }
    if (*cursor != '\0')
        return 0;
    *automatic = 0;
    return 1;
}

const char *msys_layout_profile_name(enum msys_layout_profile profile)
{
    switch (profile) {
    case MSYS_LAYOUT_KIOSK:
        return "kiosk";
    case MSYS_LAYOUT_DESKTOP:
        return "desktop";
    case MSYS_LAYOUT_MOBILE:
    default:
        return "mobile";
    }
}

const char *msys_orientation_policy_name(
        enum msys_orientation_policy orientation)
{
    switch (orientation) {
    case MSYS_ORIENTATION_PORTRAIT:
        return "portrait";
    case MSYS_ORIENTATION_LANDSCAPE:
        return "landscape";
    case MSYS_ORIENTATION_AUTO:
    default:
        return "auto";
    }
}

const char *msys_orientation_name(enum msys_orientation orientation)
{
    return orientation == MSYS_LANDSCAPE ? "landscape" : "portrait";
}

const char *msys_navigation_edge_name(enum msys_navigation_edge edge)
{
    return edge == MSYS_NAVIGATION_RIGHT ? "right" : "bottom";
}

void msys_layout_config_default(struct msys_layout_config *config)
{
    memset(config, 0, sizeof(*config));
    config->profile = MSYS_LAYOUT_MOBILE;
    config->orientation = MSYS_ORIENTATION_AUTO;
    config->automatic_insets = 1;
}

int msys_layout_config_parse(struct msys_layout_config *config,
        const char *profile, const char *orientation, const char *insets)
{
    struct msys_layout_config parsed;

    msys_layout_config_default(&parsed);
    if (!parse_profile(profile, &parsed.profile) ||
            !parse_orientation(orientation, &parsed.orientation) ||
            !parse_insets(insets, &parsed.automatic_insets, &parsed.insets))
        return 0;
    *config = parsed;
    return 1;
}

int msys_layout_config_encode(const struct msys_layout_config *config,
        char *buffer, size_t size)
{
    int length;

    if (config->automatic_insets) {
        length = snprintf(buffer, size,
                MSYS_LAYOUT_CONFIG_SCHEMA ";profile=%s;orientation=%s;insets=auto",
                msys_layout_profile_name(config->profile),
                msys_orientation_policy_name(config->orientation));
    } else {
        length = snprintf(buffer, size,
                MSYS_LAYOUT_CONFIG_SCHEMA
                ";profile=%s;orientation=%s;insets=%d,%d,%d,%d",
                msys_layout_profile_name(config->profile),
                msys_orientation_policy_name(config->orientation),
                config->insets.top, config->insets.right,
                config->insets.bottom, config->insets.left);
    }
    return length >= 0 && (size_t)length < size;
}

int msys_layout_config_decode(const char *text,
        struct msys_layout_config *config)
{
    static const char prefix[] = MSYS_LAYOUT_CONFIG_SCHEMA ";profile=";
    char profile[16];
    char orientation[16];
    char insets[64];
    char trailing;

    if (!text || strncmp(text, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    if (sscanf(text + sizeof(prefix) - 1,
                "%15[^;];orientation=%15[^;];insets=%63[^;]%c",
                profile, orientation, insets, &trailing) != 3)
        return 0;
    return msys_layout_config_parse(config, profile, orientation, insets);
}

static void clamp_insets(struct msys_insets *insets, int width, int height)
{
    int remaining;

    insets->top = clamp_int(insets->top, 0, height - 1);
    remaining = height - insets->top - 1;
    insets->bottom = clamp_int(insets->bottom, 0, remaining);
    insets->left = clamp_int(insets->left, 0, width - 1);
    remaining = width - insets->left - 1;
    insets->right = clamp_int(insets->right, 0, remaining);
}

void msys_layout_resolve(const struct msys_layout_config *config,
        int screen_width, int screen_height, struct msys_layout_state *state)
{
    int short_side;

    memset(state, 0, sizeof(*state));
    state->config = *config;
    state->screen_width = screen_width > 0 ? screen_width : 1;
    state->screen_height = screen_height > 0 ? screen_height : 1;
    if (config->orientation == MSYS_ORIENTATION_PORTRAIT)
        state->orientation = MSYS_PORTRAIT;
    else if (config->orientation == MSYS_ORIENTATION_LANDSCAPE)
        state->orientation = MSYS_LANDSCAPE;
    else
        state->orientation = state->screen_width > state->screen_height
            ? MSYS_LANDSCAPE
            : MSYS_PORTRAIT;

    short_side = state->screen_width < state->screen_height
        ? state->screen_width
        : state->screen_height;
    if (config->profile == MSYS_LAYOUT_DESKTOP)
        /* Keep the policy workarea aligned with the Native Shell's 42px
         * system bars on small touch panels.  A 24px target is both too
         * small to touch and clips the Shell's centered navigation glyphs.
         */
        state->bar_size = adaptive_metric(short_side, 24, 42, 48);
    else
        /* Mobile uses the same minimum touch target as the Shell.  In
         * particular, the OpenStick 320x480 surface must resolve to two
         * 42px bars and one 320x396 workarea; a 40px policy bar otherwise
         * resizes the role windows away from the Shell's own geometry. */
        state->bar_size = adaptive_metric(short_side, 8, 42, 64);
    state->gap = adaptive_metric(short_side, 40, 8, 24);

    if (config->automatic_insets) {
        if (config->profile == MSYS_LAYOUT_MOBILE) {
            state->insets.top = state->bar_size;
            if (state->orientation == MSYS_LANDSCAPE)
                state->insets.right = state->bar_size;
            else
                state->insets.bottom = state->bar_size;
        } else if (config->profile == MSYS_LAYOUT_DESKTOP) {
            state->insets.top = state->bar_size;
            state->insets.bottom = state->bar_size;
        }
    } else {
        state->insets = config->insets;
    }
    clamp_insets(&state->insets, state->screen_width, state->screen_height);

    state->navigation_edge = MSYS_NAVIGATION_BOTTOM;
    if (config->profile == MSYS_LAYOUT_MOBILE &&
            state->orientation == MSYS_LANDSCAPE) {
        if (config->automatic_insets || state->insets.right > 0 ||
                state->insets.bottom == 0)
            state->navigation_edge = MSYS_NAVIGATION_RIGHT;
    }
    state->workarea.x = state->insets.left;
    state->workarea.y = state->insets.top;
    state->workarea.width = state->screen_width - state->insets.left -
        state->insets.right;
    state->workarea.height = state->screen_height - state->insets.top -
        state->insets.bottom;
}

static struct msys_rect requested_or_workarea(
        const struct msys_layout_state *state, const struct msys_rect *requested)
{
    struct msys_rect result = state->workarea;

    if (requested && requested->width > 0 && requested->height > 0)
        result = *requested;
    return result;
}

static void bound_rect(const struct msys_rect *bounds, struct msys_rect *rect)
{
    int maximum_x;
    int maximum_y;

    rect->width = clamp_int(rect->width, 1, bounds->width);
    rect->height = clamp_int(rect->height, 1, bounds->height);
    maximum_x = bounds->x + bounds->width - rect->width;
    maximum_y = bounds->y + bounds->height - rect->height;
    rect->x = clamp_int(rect->x, bounds->x, maximum_x);
    rect->y = clamp_int(rect->y, bounds->y, maximum_y);
}

static void inset_rect(const struct msys_rect *source, int gap,
        struct msys_rect *result)
{
    *result = *source;
    if (result->width > gap * 2) {
        result->x += gap;
        result->width -= gap * 2;
    }
    if (result->height > gap * 2) {
        result->y += gap;
        result->height -= gap * 2;
    }
}

static void center_rect(const struct msys_rect *bounds, struct msys_rect *rect)
{
    bound_rect(bounds, rect);
    rect->x = bounds->x + (bounds->width - rect->width) / 2;
    rect->y = bounds->y + (bounds->height - rect->height) / 2;
}

void msys_layout_place(const struct msys_layout_state *state,
        enum msys_surface_kind surface, const struct msys_rect *requested,
        struct msys_rect *result)
{
    struct msys_rect available;
    int thickness;

    switch (surface) {
    case MSYS_SURFACE_INPUT_METHOD:
        *result = requested_or_workarea(state, requested);
        bound_rect(&state->workarea, result);
        break;
    case MSYS_SURFACE_CHROME:
        result->x = state->insets.left;
        result->y = 0;
        result->width = state->screen_width - state->insets.left -
            state->insets.right;
        result->height = state->insets.top > 0
            ? state->insets.top
            : state->bar_size;
        break;
    case MSYS_SURFACE_NAVIGATION:
        if (state->navigation_edge == MSYS_NAVIGATION_RIGHT) {
            thickness = state->insets.right > 0
                ? state->insets.right
                : state->bar_size;
            result->x = state->screen_width - thickness;
            result->y = state->insets.top;
            result->width = thickness;
            result->height = state->screen_height - state->insets.top -
                state->insets.bottom;
        } else {
            thickness = state->insets.bottom > 0
                ? state->insets.bottom
                : state->bar_size;
            result->x = state->insets.left;
            result->y = state->screen_height - thickness;
            result->width = state->screen_width - state->insets.left -
                state->insets.right;
            result->height = thickness;
        }
        break;
    case MSYS_SURFACE_NOTIFICATION:
        available = state->workarea;
        inset_rect(&available, state->gap, &available);
        if (state->config.profile == MSYS_LAYOUT_DESKTOP) {
            *result = requested_or_workarea(state, requested);
            result->width = requested && requested->width > 1
                ? requested->width
                : available.width * 2 / 5;
            result->height = requested && requested->height > 1
                ? requested->height
                : state->bar_size * 2;
            bound_rect(&available, result);
            result->x = available.x + available.width - result->width;
            result->y = available.y;
        } else {
            *result = available;
            result->height = requested && requested->height > 1 &&
                requested->height < available.height / 2
                ? requested->height
                : state->bar_size * 2;
            bound_rect(&available, result);
        }
        break;
    case MSYS_SURFACE_RECENTS:
        available = state->workarea;
        if (state->config.profile == MSYS_LAYOUT_DESKTOP) {
            inset_rect(&available, state->gap, &available);
            *result = requested_or_workarea(state, requested);
            center_rect(&available, result);
        } else {
            *result = available;
        }
        break;
    case MSYS_SURFACE_CHOOSER:
        available = state->workarea;
        inset_rect(&available, state->gap, &available);
        *result = requested_or_workarea(state, requested);
        if (state->config.profile != MSYS_LAYOUT_DESKTOP)
            result->width = available.width;
        center_rect(&available, result);
        break;
    case MSYS_SURFACE_TRANSITION:
        /* A launch mask follows the application coordinate space exactly in
         * every profile.  It must not inherit Overview's desktop gap or a
         * shield's root-sized geometry. */
        *result = state->workarea;
        break;
    case MSYS_SURFACE_SHIELD:
        result->x = 0;
        result->y = 0;
        result->width = state->screen_width;
        result->height = state->screen_height;
        break;
    case MSYS_SURFACE_APPLICATION:
    default:
        if (state->config.profile == MSYS_LAYOUT_DESKTOP) {
            *result = requested_or_workarea(state, requested);
            bound_rect(&state->workarea, result);
        } else {
            *result = state->workarea;
        }
        break;
    }
    if (result->width < 1)
        result->width = 1;
    if (result->height < 1)
        result->height = 1;
}

int msys_layout_effective_encode(const struct msys_layout_state *state,
        char *buffer, size_t size)
{
    char insets_policy[64];
    struct msys_rect navigation_region;
    int policy_length;

    if (state->config.automatic_insets) {
        memcpy(insets_policy, "auto", sizeof("auto"));
    } else {
        policy_length = snprintf(insets_policy, sizeof(insets_policy),
                "%d,%d,%d,%d",
                state->config.insets.top, state->config.insets.right,
                state->config.insets.bottom, state->config.insets.left);
        if (policy_length < 0 || (size_t)policy_length >= sizeof(insets_policy))
            return 0;
    }
    msys_layout_place(state, MSYS_SURFACE_NAVIGATION, NULL,
            &navigation_region);
    int length = snprintf(buffer, size,
            MSYS_LAYOUT_EFFECTIVE_SCHEMA
            ";profile=%s;orientation_policy=%s;insets_policy=%s;"
            "orientation=%s;screen=%d,%d;insets=%d,%d,%d,%d"
            ";workarea=%d,%d,%d,%d;navigation=%s"
            ";navigation_region=%d,%d,%d,%d",
            msys_layout_profile_name(state->config.profile),
            msys_orientation_policy_name(state->config.orientation),
            insets_policy,
            msys_orientation_name(state->orientation),
            state->screen_width, state->screen_height,
            state->insets.top, state->insets.right,
            state->insets.bottom, state->insets.left,
            state->workarea.x, state->workarea.y,
            state->workarea.width, state->workarea.height,
            msys_navigation_edge_name(state->navigation_edge),
            navigation_region.x, navigation_region.y,
            navigation_region.width, navigation_region.height);

    return length >= 0 && (size_t)length < size;
}
