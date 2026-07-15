#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>

enum cleanup_event {
    CLEANUP_DISPLAY = 1,
    CLEANUP_LIBRARY = 2
};

static enum cleanup_event cleanup_events[2];
static int cleanup_event_count;

static int test_XCloseDisplay(Display *display)
{
    assert(display != NULL);
    assert(cleanup_event_count < 2);
    cleanup_events[cleanup_event_count++] = CLEANUP_DISPLAY;
    return 0;
}

static int test_dlclose(void *library)
{
    assert(library != NULL);
    assert(cleanup_event_count < 2);
    cleanup_events[cleanup_event_count++] = CLEANUP_LIBRARY;
    return 0;
}

#define XCloseDisplay test_XCloseDisplay
#define dlclose test_dlclose
#define main msys_x11_policy_program_main
#include "../src/msys_x11_policy.c"
#undef main
#undef dlclose
#undef XCloseDisplay

static struct window_metadata metadata(const char *title, const char *wm_class,
        const char *app_id, const char *component_id, const char *role)
{
    struct window_metadata value;

    memset(&value, 0, sizeof(value));
    value.title = (char *)title;
    value.wm_class = (char *)wm_class;
    value.app_id = (char *)app_id;
    value.component_id = (char *)component_id;
    value.role = (char *)role;
    return value;
}

static void test_metadata_damage_filters(void)
{
    const Atom properties[] = {11, 22, None, 44};
    XWindowAttributes attributes;
    struct msys_rect rect;

    memset(&attributes, 0, sizeof(attributes));
    attributes.x = 3;
    attributes.y = 7;
    attributes.width = 320;
    attributes.height = 396;
    rect.x = 3;
    rect.y = 7;
    rect.width = 320;
    rect.height = 396;
    assert(geometry_matches(&attributes, &rect));
    rect.y++;
    assert(!geometry_matches(&attributes, &rect));
    assert(property_affects_window_metadata(11, properties, 4));
    assert(property_affects_window_metadata(44, properties, 4));
    assert(!property_affects_window_metadata(33, properties, 4));
    assert(!property_affects_window_metadata(None, properties, 4));
    assert(overlay_layer_pair_is_ordered(LAYER_APPLICATION, LAYER_INPUT_METHOD));
    assert(overlay_layer_pair_is_ordered(LAYER_INPUT_METHOD, LAYER_RECENTS));
    assert(overlay_layer_pair_is_ordered(LAYER_CHROME, LAYER_NAVIGATION));
    assert(!overlay_layer_pair_is_ordered(LAYER_RECENTS, LAYER_APPLICATION));
    assert(!overlay_layer_pair_is_ordered(LAYER_RECENTS, LAYER_INPUT_METHOD));
}

static void test_stable_identity_precedes_title(void)
{
    struct window_metadata value = metadata("A translated status title",
            "org.msys.shell.system-chrome", NULL, NULL, NULL);

    assert(classify_window(&value) == WINDOW_CHROME);
    value.title = "MSYS Recents - misleading presentation text";
    assert(classify_window(&value) == WINDOW_CHROME);
}

static void test_all_reference_identities(void)
{
    struct window_metadata value = metadata("changed", NULL,
            "org.msys.shell.launcher", NULL, NULL);

    assert(classify_window(&value) == WINDOW_LAUNCHER);
    value.app_id = "org.msys.input.touch";
    assert(classify_window(&value) == WINDOW_INPUT_METHOD);
    value.app_id = "org.msys.shell.task-switcher";
    assert(classify_window(&value) == WINDOW_RECENTS);
    value.app_id = "org.msys.shell.notification-center";
    assert(classify_window(&value) == WINDOW_RECENTS);
    value.app_id = "org.msys.shell.intent-chooser";
    assert(classify_window(&value) == WINDOW_CHOOSER);
    value.app_id = "org.msys.shell.notifications";
    assert(classify_window(&value) == WINDOW_NOTIFICATION);
    value.app_id = "org.msys.shell.navigation-pill";
    assert(classify_window(&value) == WINDOW_NAVIGATION);
    value.app_id = "org.msys.shell.transitions";
    assert(classify_window(&value) == WINDOW_TRANSITION);
    value.app_id = "org.msys.shell.screen-shield";
    assert(classify_window(&value) == WINDOW_SHIELD);
}

static void test_explicit_role_supports_replaceable_provider(void)
{
    struct window_metadata value = metadata("Vendor UI", "com.vendor.panel",
            "com.vendor.panel", NULL, "role:navigation-bar");

    assert(classify_window(&value) == WINDOW_NAVIGATION);
    value.role = "notification-presenter";
    assert(classify_window(&value) == WINDOW_NOTIFICATION);
    value.role = "notification-center";
    assert(classify_window(&value) == WINDOW_RECENTS);
    value.role = "chooser";
    assert(classify_window(&value) == WINDOW_CHOOSER);
    value.role = "transition-presenter";
    assert(classify_window(&value) == WINDOW_TRANSITION);
    value.role = "application";
    value.title = "MSYS Screen Shield compatibility title";
    assert(classify_window(&value) == WINDOW_APPLICATION);

    value.title = "Vendor keyboard";
    value.role = "input-method";
    assert(classify_window(&value) == WINDOW_INPUT_METHOD);
    assert(strcmp(window_kind_name(classify_window(&value)), "overlay") == 0);
    value.role = "role:virtual-keyboard";
    assert(classify_window(&value) == WINDOW_INPUT_METHOD);
    value.role = "on-screen-keyboard";
    assert(classify_window(&value) == WINDOW_INPUT_METHOD);
    value.role = "soft-keyboard";
    assert(classify_window(&value) == WINDOW_INPUT_METHOD);
}

static void test_configured_provider_identity(void)
{
    struct window_metadata value = metadata("Vendor top panel",
            "com.vendor.top-panel", NULL, NULL, NULL);

    assert(setenv("MSYS_X11_CHROME_IDENTITIES",
                " org.example.other, com.vendor.top-panel ", 1) == 0);
    assert(classify_window(&value) == WINDOW_CHROME);
    assert(unsetenv("MSYS_X11_CHROME_IDENTITIES") == 0);

    assert(setenv("MSYS_X11_INPUT_METHOD_IDENTITIES",
                "com.vendor.top-panel", 1) == 0);
    assert(classify_window(&value) == WINDOW_INPUT_METHOD);
    assert(unsetenv("MSYS_X11_INPUT_METHOD_IDENTITIES") == 0);
}

static void test_legacy_title_fallback(void)
{
    struct window_metadata value = metadata("MSYS Notifications - toast",
            "Tk", NULL, NULL, NULL);

    assert(classify_window(&value) == WINDOW_NOTIFICATION);
    value.title = "MSYS Navigation";
    assert(classify_window(&value) == WINDOW_NAVIGATION);
    value.title = "MSYS Intent Chooser";
    assert(classify_window(&value) == WINDOW_CHOOSER);
    value.title = "Ordinary application";
    assert(classify_window(&value) == WINDOW_APPLICATION);
}

static void test_canonical_overlay_role_avoids_title_matching(void)
{
    struct window_metadata value = metadata("Localized", NULL,
            "org.msys.shell.notification-center", NULL, NULL);
    int compatibility_title = -1;

    assert(strcmp(canonical_window_role(&value, classify_window(&value),
                    &compatibility_title), "notification-center") == 0);
    assert(compatibility_title == 0);
    value.app_id = "org.vendor.overlay";
    value.role = "role:task-switcher";
    assert(strcmp(canonical_window_role(&value, classify_window(&value),
                    &compatibility_title), "task-switcher") == 0);
    assert(compatibility_title == 0);
    value.role = NULL;
    value.title = "MSYS Screen Shield (old)";
    assert(strcmp(canonical_window_role(&value, classify_window(&value),
                    &compatibility_title), "screen-shield") == 0);
    assert(compatibility_title == 1);

    value.title = "Localized keyboard";
    value.app_id = "org.msys.input.touch";
    assert(strcmp(canonical_window_role(&value, classify_window(&value),
                    &compatibility_title), "input-method") == 0);
    assert(compatibility_title == 0);
    value.app_id = "org.vendor.keyboard";
    value.role = "role:soft-keyboard";
    assert(strcmp(canonical_window_role(&value, classify_window(&value),
                    &compatibility_title), "input-method") == 0);
    assert(compatibility_title == 0);
}

static void test_stable_window_id_parser_rejects_stale_shapes(void)
{
    Window window = None;

    assert(parse_window_id(
                "msys.x11-window.v1:abc-123:0x2a", &window));
    assert(window == (Window)0x2a);
    assert(!parse_window_id("x11:0x2a", &window));
    assert(!parse_window_id("msys.x11-window.v1::0x2a", &window));
    assert(!parse_window_id("msys.x11-window.v1:abc-123:0", &window));
    assert(!parse_window_id("msys.x11-window.v1:abc-123:0x2a-tail",
                &window));
}

static void test_identity_matching_is_exact_and_case_insensitive(void)
{
    struct window_metadata value = metadata("app", "Example.App", NULL,
            "org.example:main", NULL);

    assert(metadata_matches_identity(&value, "example.app"));
    assert(metadata_matches_identity(&value, "ORG.EXAMPLE:MAIN"));
    assert(!metadata_matches_identity(&value, "Example"));
    assert(!metadata_matches_identity(&value, "Example.App.child"));
    value.declared_identity = "org.example.injected";
    assert(metadata_matches_identity(&value, "ORG.EXAMPLE.INJECTED"));
}

static void test_bounded_process_environment_extraction(void)
{
    static const unsigned char environment[] =
        "IGNORED=secret\0"
        "MSYS_APP_ID=org.example.from-environ\0"
        "MSYS_COMPONENT_ID=org.example:main\0"
        "MSYS_WINDOW_IDENTITY=ExampleWindow\0"
        "MSYS_WINDOW_ROLE=chooser\0";
    struct window_metadata value = metadata("title", NULL, NULL, NULL, NULL);

    apply_process_environment(&value, environment, sizeof(environment) - 1);
    assert(strcmp(value.process_app_id, "org.example.from-environ") == 0);
    assert(strcmp(value.process_component_id, "org.example:main") == 0);
    assert(strcmp(value.declared_identity, "ExampleWindow") == 0);
    assert(strcmp(value.role, "chooser") == 0);
    assert(metadata_matches_identity(&value, "org.example.from-environ"));
    assert(metadata_matches_identity(&value, "org.example:main"));
    assert(metadata_matches_identity(&value, "ExampleWindow"));
    assert(classify_window(&value) == WINDOW_CHOOSER);
    free(value.process_app_id);
    free(value.process_component_id);
    free(value.declared_identity);
    free(value.role);
}

static void test_truncated_environment_entry_is_rejected(void)
{
    static const unsigned char truncated[] = "MSYS_APP_ID=not-terminated";

    assert(environment_value(truncated, sizeof(truncated) - 1,
                "MSYS_APP_ID") == NULL);
}

static void test_layer_order(void)
{
    assert(!window_kind_allows_override_redirect(WINDOW_APPLICATION));
    assert(!window_kind_allows_override_redirect(WINDOW_NOTIFICATION));
    assert(window_kind_allows_override_redirect(WINDOW_INPUT_METHOD));
    assert(surface_for_window_kind(WINDOW_INPUT_METHOD) ==
            MSYS_SURFACE_INPUT_METHOD);
    assert(layer_for_kind(WINDOW_APPLICATION) == LAYER_APPLICATION);
    assert(layer_for_kind(WINDOW_LAUNCHER) == LAYER_APPLICATION);
    assert(layer_for_kind(WINDOW_APPLICATION) <
            layer_for_kind(WINDOW_INPUT_METHOD));
    assert(layer_for_kind(WINDOW_INPUT_METHOD) < layer_for_kind(WINDOW_RECENTS));
    assert(layer_for_kind(WINDOW_RECENTS) < layer_for_kind(WINDOW_CHOOSER));
    assert(layer_for_kind(WINDOW_CHOOSER) < layer_for_kind(WINDOW_NOTIFICATION));
    assert(layer_for_kind(WINDOW_NOTIFICATION) < layer_for_kind(WINDOW_CHROME));
    assert(layer_for_kind(WINDOW_CHROME) < layer_for_kind(WINDOW_NAVIGATION));
    assert(layer_for_kind(WINDOW_NAVIGATION) < layer_for_kind(WINDOW_TRANSITION));
    assert(layer_for_kind(WINDOW_TRANSITION) < layer_for_kind(WINDOW_SHIELD));
}

static void test_debug_coordinates_are_strict(void)
{
    int value = -1;

    assert(parse_coordinate("0", &value) && value == 0);
    assert(parse_coordinate("319", &value) && value == 319);
    assert(!parse_coordinate("-1", &value));
    assert(!parse_coordinate("12px", &value));
    assert(!parse_coordinate("", &value));
    assert(parse_signed_coordinate("-32768", &value) && value == -32768);
    assert(parse_signed_coordinate("32767", &value) && value == 32767);
    assert(!parse_signed_coordinate("32768", &value));
    assert(!parse_signed_coordinate("-32769", &value));
    assert(parse_dimension("1", &value) && value == 1);
    assert(!parse_dimension("0", &value));
}

static void test_debug_gesture_contract_is_strict_and_interpolated(void)
{
    int duration = -1;

    assert(debug_gesture_selector_for_option("--debug-swipe-identity") ==
            DEBUG_GESTURE_IDENTITY);
    assert(debug_gesture_selector_for_option("--debug-swipe-title") ==
            DEBUG_GESTURE_TITLE);
    assert(debug_gesture_selector_for_option("--debug-swipe-window") ==
            DEBUG_GESTURE_WINDOW);
    assert(debug_gesture_selector_for_option("--debug-drag-identity") ==
            DEBUG_GESTURE_IDENTITY);
    assert(debug_gesture_selector_for_option("--debug-drag-title") ==
            DEBUG_GESTURE_TITLE);
    assert(debug_gesture_selector_for_option("--debug-drag-window") ==
            DEBUG_GESTURE_WINDOW);
    assert(debug_gesture_selector_for_option("--debug-swipe") ==
            DEBUG_GESTURE_NONE);
    assert(valid_debug_gesture_target("org.example.app", NULL));
    assert(valid_debug_gesture_target(NULL, "Example"));
    assert(valid_debug_gesture_target("org.example.app", "Example"));
    assert(!valid_debug_gesture_target("", NULL));
    assert(!valid_debug_gesture_target(NULL, ""));

    assert(parse_gesture_duration("1", &duration) && duration == 1);
    assert(parse_gesture_duration("10000", &duration) && duration == 10000);
    assert(!parse_gesture_duration("0", &duration));
    assert(!parse_gesture_duration("10001", &duration));
    assert(!parse_gesture_duration("16ms", &duration));
    assert(!parse_gesture_duration("-1", &duration));
    assert(gesture_step_count(1) == 1);
    assert(gesture_step_count(16) == 1);
    assert(gesture_step_count(17) == 2);
    assert(gesture_step_count(10000) == 625);
    assert(interpolate_coordinate(0, 100, 1, 4) == 25);
    assert(interpolate_coordinate(100, 0, 1, 4) == 75);
    assert(interpolate_coordinate(100, 0, 4, 4) == 0);
}

static void test_debug_gesture_closes_display_before_unloading_xtest(void)
{
    struct debug_xtest_api api = {0};
    char display_token;
    char library_token;

    api.library = &library_token;
    cleanup_event_count = 0;
    debug_gesture_close((Display *)&display_token, &api);

    assert(cleanup_event_count == 2);
    assert(cleanup_events[0] == CLEANUP_DISPLAY);
    assert(cleanup_events[1] == CLEANUP_LIBRARY);
    assert(api.library == NULL);
    assert(api.query_extension == NULL);
    assert(api.fake_motion == NULL);
    assert(api.fake_button == NULL);
}

static void test_debug_gesture_cleanup_accepts_an_unopened_xtest_api(void)
{
    struct debug_xtest_api api = {0};
    char display_token;

    cleanup_event_count = 0;
    debug_gesture_close((Display *)&display_token, &api);

    assert(cleanup_event_count == 1);
    assert(cleanup_events[0] == CLEANUP_DISPLAY);
}

static void clear_layout_environment(void)
{
    unsetenv("MSYS_LAYOUT_PROFILE");
    unsetenv("MSYS_ORIENTATION");
    unsetenv("MSYS_INSETS");
    unsetenv("MSYS_WINDOW_POLICY");
    unsetenv("MSYS_CHROME_TOP");
    unsetenv("MSYS_CHROME_BOTTOM");
}

static void test_layout_environment_contract_and_legacy_bridge(void)
{
    struct msys_layout_config config;

    clear_layout_environment();
    assert(setenv("MSYS_WINDOW_POLICY", "mobile", 1) == 0);
    assert(setenv("MSYS_CHROME_TOP", "42", 1) == 0);
    assert(setenv("MSYS_CHROME_BOTTOM", "37", 1) == 0);
    assert(layout_config_from_environment(&config));
    assert(config.profile == MSYS_LAYOUT_MOBILE);
    assert(!config.automatic_insets);
    assert(config.insets.top == 42 && config.insets.bottom == 37);

    assert(setenv("MSYS_LAYOUT_PROFILE", "kiosk", 1) == 0);
    assert(setenv("MSYS_ORIENTATION", "auto", 1) == 0);
    assert(setenv("MSYS_INSETS", "auto", 1) == 0);
    assert(layout_config_from_environment(&config));
    assert(config.profile == MSYS_LAYOUT_KIOSK);
    assert(config.automatic_insets);

    assert(setenv("MSYS_INSETS", "42,0,broken,0", 1) == 0);
    assert(!layout_config_from_environment(&config));
    clear_layout_environment();
}

static void test_display_session_layout_signal_is_strict(void)
{
    struct display_layout_signal signal;
    struct display_layout_signal decoded;
    char encoded[384];

    assert(display_layout_signal_init(&signal, "320", "480", "24", "1",
                "0,-1,1,1,0,0,0,0,1"));
    assert(display_layout_signal_encode(&signal, encoded, sizeof(encoded)));
    assert(display_layout_signal_decode(encoded, &decoded));
    assert(decoded.width == 320 && decoded.height == 480);
    assert(decoded.depth == 24 && decoded.input_enabled == 1);
    assert(strcmp(decoded.input_matrix, "0,-1,1,1,0,0,0,0,1") == 0);

    assert(display_layout_signal_init(&signal, "1920", "1080", "24", "0",
                "none"));
    assert(!display_layout_signal_init(&signal, "0", "480", "24", "1",
                "1,0,0,0,1,0,0,0,1"));
    assert(!display_layout_signal_init(&signal, "320", "480", "24", "1",
                "1,0,0"));
    assert(!display_layout_signal_init(&signal, "320", "480", "24", "1",
                "1,0,0,0,0,0,0,0,1"));
    assert(!display_layout_signal_init(&signal, "320", "480", "24", "0",
                "1,0,0,0,1,0,0,0,1"));
    assert(!display_layout_signal_decode(
                "msys.display-session.layout.v1;screen=320,480,24;"
                "input_enabled=1;input_matrix=1,0,0,0,1,0,0,0,0",
                &decoded));
}

static void test_thumbnail_scaling_and_rgb_masks(void)
{
    int width;
    int height;

    thumbnail_dimensions(320, 480, &width, &height);
    assert(width == 240 && height == 360);
    thumbnail_dimensions(1920, 1080, &width, &height);
    assert(width == 288 && height == 162);
    thumbnail_dimensions(64, 32, &width, &height);
    assert(width == 64 && height == 32);
    assert(thumbnail_channel(0x00ff0000UL, 0x00ff0000UL) == 255);
    assert(thumbnail_channel(0x00008000UL, 0x0000ff00UL) == 128);
    assert(thumbnail_channel(0x00000000UL, 0x000000ffUL) == 0);
    assert(thumbnail_channel(0x0000f800UL, 0x0000f800UL) == 255);
}

static void test_thumbnail_refresh_is_frozen_behind_task_switcher(void)
{
    XWindowAttributes left;
    XWindowAttributes right;

    assert(thumbnail_refresh_allowed(IsViewable, 0, 0));
    assert(!thumbnail_refresh_allowed(IsViewable, 1, 0));
    assert(!thumbnail_refresh_allowed(IsViewable, 0, 1));
    assert(!thumbnail_refresh_allowed(IsUnmapped, 0, 0));
    assert(!thumbnail_refresh_allowed(IsUnviewable, 0, 0));
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    left.x = 0;
    left.y = 42;
    left.width = 320;
    left.height = 396;
    right.x = 0;
    right.y = 0;
    right.width = 320;
    right.height = 42;
    assert(!thumbnail_rectangles_overlap(&left, &right));
    right.y = 40;
    assert(thumbnail_rectangles_overlap(&left, &right));
    right.x = 320;
    assert(!thumbnail_rectangles_overlap(&left, &right));
}

int main(void)
{
    test_metadata_damage_filters();
    test_stable_identity_precedes_title();
    test_all_reference_identities();
    test_explicit_role_supports_replaceable_provider();
    test_configured_provider_identity();
    test_legacy_title_fallback();
    test_canonical_overlay_role_avoids_title_matching();
    test_stable_window_id_parser_rejects_stale_shapes();
    test_identity_matching_is_exact_and_case_insensitive();
    test_bounded_process_environment_extraction();
    test_truncated_environment_entry_is_rejected();
    test_layer_order();
    test_debug_coordinates_are_strict();
    test_debug_gesture_contract_is_strict_and_interpolated();
    test_debug_gesture_closes_display_before_unloading_xtest();
    test_debug_gesture_cleanup_accepts_an_unopened_xtest_api();
    test_layout_environment_contract_and_legacy_bridge();
    test_display_session_layout_signal_is_strict();
    test_thumbnail_scaling_and_rgb_masks();
    test_thumbnail_refresh_is_frozen_behind_task_switcher();
    puts("test_policy_logic: ok");
    return 0;
}
