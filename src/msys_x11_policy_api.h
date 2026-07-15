#ifndef MSYS_X11_POLICY_API_H
#define MSYS_X11_POLICY_API_H

#include <stddef.h>

#define MSYS_X11_SUMMARY_TEXT_MAX 256
#define MSYS_X11_SUMMARY_ID_MAX 192

struct msys_x11_window_summary {
    char window_id[MSYS_X11_SUMMARY_ID_MAX];
    char title[MSYS_X11_SUMMARY_TEXT_MAX];
    char identity[MSYS_X11_SUMMARY_TEXT_MAX];
    char component[MSYS_X11_SUMMARY_TEXT_MAX];
    char role[64];
    char kind[32];
};

/*
 * In-process operations used by the native mIPC provider.  Returned strings
 * are allocated with malloc(3) and belong to the caller.  Keeping this small
 * boundary means the provider and the X11 policy share one ELF and one PID;
 * no command helper is spawned for normal runtime operations.
 */
char *msys_x11_policy_list_windows_json(const char *display_name);
char *msys_x11_policy_layout_wire(const char *display_name);
int msys_x11_policy_top_window(const char *display_name,
        int dismissible_only, struct msys_x11_window_summary *summary);
int msys_x11_policy_component_window(const char *display_name,
        const char *component, struct msys_x11_window_summary *summary);
int msys_x11_policy_window_summary(const char *display_name,
        const char *window_id, struct msys_x11_window_summary *summary);
int msys_x11_policy_window_action(const char *display_name,
        const char *action, const char *window_id,
        int x, int y, int width, int height);
int msys_x11_policy_activate(const char *display_name,
        const char *identity, const char *title);
int msys_x11_policy_close_identity(const char *display_name,
        const char *identity, const char *title);
int msys_x11_policy_set_layout(const char *display_name,
        const char *profile, const char *orientation, const char *insets);
int msys_x11_policy_sync_display(const char *display_name,
        int width, int height, int depth, int input_enabled,
        const char *input_matrix);

/* Provider-side, atomic msys.display-session.v1 publisher. */
int msys_x11_publish_display_session(const char *display_name,
        const char *provider, const char *state_file);

#endif
