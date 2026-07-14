#include "../src/msys_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct msys_layout_config config(const char *profile,
        const char *orientation, const char *insets)
{
    struct msys_layout_config result;

    assert(msys_layout_config_parse(&result, profile, orientation, insets));
    return result;
}

static void test_contract_round_trip_and_strict_rejection(void)
{
    struct msys_layout_config expected = config("mobile", "landscape",
            "8,9,10,11");
    struct msys_layout_config decoded;
    char encoded[MSYS_LAYOUT_TEXT_MAX];

    assert(msys_layout_config_encode(&expected, encoded, sizeof(encoded)));
    assert(strcmp(encoded,
                "msys.layout.v1;profile=mobile;orientation=landscape;"
                "insets=8,9,10,11") == 0);
    assert(msys_layout_config_decode(encoded, &decoded));
    assert(decoded.profile == expected.profile);
    assert(decoded.orientation == expected.orientation);
    assert(!decoded.automatic_insets);
    assert(decoded.insets.right == 9);

    assert(!msys_layout_config_parse(&decoded, "phone", "auto", "auto"));
    assert(!msys_layout_config_parse(&decoded, "Mobile", "auto", "auto"));
    assert(!msys_layout_config_parse(&decoded, "mobile", "sideways", "auto"));
    assert(!msys_layout_config_parse(&decoded, "mobile", "auto", "1,2,3"));
    assert(!msys_layout_config_parse(&decoded, "mobile", "auto", "1,2,3,4x"));
    assert(!msys_layout_config_decode(
                "msys.layout.v2;profile=mobile;orientation=auto;insets=auto",
                &decoded));
}

static void test_mobile_portrait_is_resolution_independent(void)
{
    struct msys_layout_config input = config("mobile", "auto", "auto");
    struct msys_layout_state state;
    struct msys_rect app;
    struct msys_rect chrome;
    struct msys_rect navigation;

    msys_layout_resolve(&input, 360, 800, &state);
    assert(state.orientation == MSYS_PORTRAIT);
    assert(state.insets.top == state.bar_size);
    assert(state.insets.bottom == state.bar_size);
    assert(state.insets.right == 0);
    assert(state.workarea.width == 360);
    assert(state.workarea.height == 800 - state.bar_size * 2);
    msys_layout_place(&state, MSYS_SURFACE_APPLICATION, NULL, &app);
    msys_layout_place(&state, MSYS_SURFACE_CHROME, NULL, &chrome);
    msys_layout_place(&state, MSYS_SURFACE_NAVIGATION, NULL, &navigation);
    assert(app.x == 0 && app.y == state.bar_size);
    assert(chrome.width == 360 && chrome.height == state.bar_size);
    assert(navigation.y == 800 - state.bar_size);
    assert(navigation.width == 360);
}

static void test_mobile_spi_matches_shell_workarea(void)
{
    struct msys_layout_config input = config("mobile", "portrait", "auto");
    struct msys_layout_state state;
    struct msys_rect chrome;
    struct msys_rect navigation;
    struct msys_rect application;
    struct msys_rect recents;
    struct msys_rect requested_recents = {8, 50, 304, 380};

    msys_layout_resolve(&input, 320, 480, &state);
    assert(state.bar_size == 42);
    assert(state.workarea.x == 0 && state.workarea.y == 42);
    assert(state.workarea.width == 320 && state.workarea.height == 396);

    msys_layout_place(&state, MSYS_SURFACE_CHROME, NULL, &chrome);
    msys_layout_place(&state, MSYS_SURFACE_NAVIGATION, NULL, &navigation);
    msys_layout_place(&state, MSYS_SURFACE_APPLICATION, NULL, &application);
    msys_layout_place(&state, MSYS_SURFACE_RECENTS, &requested_recents,
            &recents);
    assert(chrome.x == 0 && chrome.y == 0 && chrome.width == 320 &&
            chrome.height == 42);
    assert(navigation.x == 0 && navigation.y == 438 &&
            navigation.width == 320 && navigation.height == 42);
    assert(memcmp(&application, &state.workarea, sizeof(application)) == 0);
    /* Mobile ignores a stale desktop-sized request: Overview must cover the
     * complete application workarea so no underlying toolkit gutter leaks. */
    assert(memcmp(&recents, &state.workarea, sizeof(recents)) == 0);
}

static void test_mobile_landscape_moves_navigation_to_right_edge(void)
{
    struct msys_layout_config input = config("mobile", "auto", "auto");
    struct msys_layout_state state;
    struct msys_rect app;
    struct msys_rect navigation;

    msys_layout_resolve(&input, 1280, 720, &state);
    assert(state.orientation == MSYS_LANDSCAPE);
    assert(state.navigation_edge == MSYS_NAVIGATION_RIGHT);
    assert(state.insets.top == state.bar_size);
    assert(state.insets.right == state.bar_size);
    assert(state.insets.bottom == 0);
    msys_layout_place(&state, MSYS_SURFACE_APPLICATION, NULL, &app);
    msys_layout_place(&state, MSYS_SURFACE_NAVIGATION, NULL, &navigation);
    assert(app.width == 1280 - state.bar_size);
    assert(navigation.x == 1280 - state.bar_size);
    assert(navigation.y == state.bar_size);
    assert(navigation.height == 720 - state.bar_size);
}

static void test_auto_orientation_reacts_to_root_size_change(void)
{
    struct msys_layout_config input = config("mobile", "auto", "auto");
    struct msys_layout_state state;

    msys_layout_resolve(&input, 480, 800, &state);
    assert(state.orientation == MSYS_PORTRAIT);
    assert(state.navigation_edge == MSYS_NAVIGATION_BOTTOM);
    msys_layout_resolve(&input, 800, 480, &state);
    assert(state.orientation == MSYS_LANDSCAPE);
    assert(state.navigation_edge == MSYS_NAVIGATION_RIGHT);
    assert(state.workarea.width == 800 - state.bar_size);
}

static void test_kiosk_uses_the_complete_root_window(void)
{
    struct msys_layout_config input = config("kiosk", "auto", "auto");
    struct msys_layout_state state;
    struct msys_rect requested = {20, 30, 300, 200};
    struct msys_rect app;

    msys_layout_resolve(&input, 1024, 600, &state);
    msys_layout_place(&state, MSYS_SURFACE_APPLICATION, &requested, &app);
    assert(state.insets.top == 0 && state.insets.right == 0);
    assert(state.insets.bottom == 0 && state.insets.left == 0);
    assert(app.x == 0 && app.y == 0);
    assert(app.width == 1024 && app.height == 600);
}

static void test_desktop_preserves_and_bounds_client_geometry(void)
{
    struct msys_layout_config input = config("desktop", "landscape", "auto");
    struct msys_layout_state state;
    struct msys_rect requested = {1800, 1000, 700, 500};
    struct msys_rect app;

    msys_layout_resolve(&input, 1920, 1080, &state);
    msys_layout_place(&state, MSYS_SURFACE_APPLICATION, &requested, &app);
    assert(app.width == 700 && app.height == 500);
    assert(app.x + app.width <= state.workarea.x + state.workarea.width);
    assert(app.y + app.height <= state.workarea.y + state.workarea.height);
    assert(app.x >= state.workarea.x && app.y >= state.workarea.y);
}

static void test_desktop_spi_uses_shell_sized_system_bars(void)
{
    struct msys_layout_config input = config("desktop", "portrait", "auto");
    struct msys_layout_state state;
    struct msys_rect chrome;
    struct msys_rect navigation;
    struct msys_rect app;

    msys_layout_resolve(&input, 320, 480, &state);
    assert(state.bar_size == 42);
    assert(state.insets.top == 42 && state.insets.bottom == 42);
    assert(state.workarea.x == 0 && state.workarea.y == 42);
    assert(state.workarea.width == 320 && state.workarea.height == 396);
    msys_layout_place(&state, MSYS_SURFACE_CHROME, NULL, &chrome);
    msys_layout_place(&state, MSYS_SURFACE_NAVIGATION, NULL, &navigation);
    msys_layout_place(&state, MSYS_SURFACE_APPLICATION, NULL, &app);
    assert(chrome.x == 0 && chrome.y == 0 && chrome.width == 320 && chrome.height == 42);
    assert(navigation.x == 0 && navigation.y == 438 && navigation.width == 320 &&
            navigation.height == 42);
    assert(app.x == 0 && app.y == 42 && app.width == 320 && app.height == 396);
}

static void test_mobile_input_method_preserves_floating_geometry(void)
{
    struct msys_layout_config input = config("mobile", "portrait", "auto");
    struct msys_layout_state state;
    struct msys_rect requested = {20, 480, 320, 240};
    struct msys_rect keyboard;

    msys_layout_resolve(&input, 360, 800, &state);
    msys_layout_place(&state, MSYS_SURFACE_INPUT_METHOD, &requested,
            &keyboard);
    assert(keyboard.x == requested.x);
    assert(keyboard.y == requested.y);
    assert(keyboard.width == requested.width);
    assert(keyboard.height == requested.height);
    assert(keyboard.width < state.workarea.width);
    assert(keyboard.height < state.workarea.height);
}

static void test_mobile_recents_owns_the_complete_workarea(void)
{
    struct msys_layout_config mobile = config("mobile", "portrait", "auto");
    struct msys_layout_config kiosk = config("kiosk", "portrait", "auto");
    struct msys_layout_config desktop = config("desktop", "portrait", "auto");
    struct msys_layout_state state;
    struct msys_rect requested = {30, 80, 220, 260};
    struct msys_rect recents;

    msys_layout_resolve(&mobile, 320, 480, &state);
    msys_layout_place(&state, MSYS_SURFACE_RECENTS, &requested, &recents);
    assert(memcmp(&recents, &state.workarea, sizeof(recents)) == 0);

    msys_layout_resolve(&kiosk, 320, 480, &state);
    msys_layout_place(&state, MSYS_SURFACE_RECENTS, &requested, &recents);
    assert(memcmp(&recents, &state.workarea, sizeof(recents)) == 0);

    msys_layout_resolve(&desktop, 320, 480, &state);
    msys_layout_place(&state, MSYS_SURFACE_RECENTS, &requested, &recents);
    assert(recents.width == requested.width && recents.height == requested.height);
    assert(recents.x > state.workarea.x && recents.y > state.workarea.y);
}

static void test_input_method_is_bounded_to_workarea(void)
{
    struct msys_layout_config input = config("mobile", "portrait", "auto");
    struct msys_layout_state state;
    struct msys_rect requested = {-100, 760, 900, 400};
    struct msys_rect keyboard;

    msys_layout_resolve(&input, 360, 800, &state);
    msys_layout_place(&state, MSYS_SURFACE_INPUT_METHOD, &requested,
            &keyboard);
    assert(keyboard.x == state.workarea.x);
    assert(keyboard.width == state.workarea.width);
    assert(keyboard.height == requested.height);
    assert(keyboard.y + keyboard.height ==
            state.workarea.y + state.workarea.height);
    assert(keyboard.height < state.workarea.height);
}

static void test_explicit_insets_are_clamped_to_a_valid_workarea(void)
{
    struct msys_layout_config input = config("mobile", "portrait",
            "999,999,999,999");
    struct msys_layout_state state;

    msys_layout_resolve(&input, 20, 10, &state);
    assert(state.workarea.width >= 1);
    assert(state.workarea.height >= 1);
    assert(state.insets.left + state.insets.right < 20);
    assert(state.insets.top + state.insets.bottom < 10);
}

static void test_explicit_edge_selects_compatible_navigation(void)
{
    struct msys_layout_state state;
    struct msys_layout_config legacy = config("mobile", "auto", "42,0,42,0");
    struct msys_layout_config right = config("mobile", "auto", "42,42,0,0");

    msys_layout_resolve(&legacy, 800, 480, &state);
    assert(state.orientation == MSYS_LANDSCAPE);
    assert(state.navigation_edge == MSYS_NAVIGATION_BOTTOM);
    msys_layout_resolve(&right, 800, 480, &state);
    assert(state.navigation_edge == MSYS_NAVIGATION_RIGHT);
}

static void test_effective_state_is_machine_readable(void)
{
    struct msys_layout_config input = config("mobile", "auto", "auto");
    struct msys_layout_state state;
    char encoded[MSYS_LAYOUT_TEXT_MAX];

    msys_layout_resolve(&input, 800, 480, &state);
    assert(msys_layout_effective_encode(&state, encoded, sizeof(encoded)));
    assert(strstr(encoded, "msys.layout.effective.v1;") == encoded);
    assert(strstr(encoded, "orientation_policy=auto") != NULL);
    assert(strstr(encoded, "insets_policy=auto") != NULL);
    assert(strstr(encoded, "orientation=landscape") != NULL);
    assert(strstr(encoded, "screen=800,480") != NULL);
    assert(strstr(encoded, "navigation=right") != NULL);
    assert(strstr(encoded, "navigation_region=740,60,60,420") != NULL);
}

int main(void)
{
    test_contract_round_trip_and_strict_rejection();
    test_mobile_portrait_is_resolution_independent();
    test_mobile_spi_matches_shell_workarea();
    test_mobile_landscape_moves_navigation_to_right_edge();
    test_auto_orientation_reacts_to_root_size_change();
    test_kiosk_uses_the_complete_root_window();
    test_desktop_preserves_and_bounds_client_geometry();
    test_desktop_spi_uses_shell_sized_system_bars();
    test_mobile_input_method_preserves_floating_geometry();
    test_mobile_recents_owns_the_complete_workarea();
    test_input_method_is_bounded_to_workarea();
    test_explicit_insets_are_clamped_to_a_valid_workarea();
    test_explicit_edge_selects_compatible_navigation();
    test_effective_state_is_machine_readable();
    puts("test_layout: ok");
    return 0;
}
