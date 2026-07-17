#ifndef MSYS_X11_AGENT_H
#define MSYS_X11_AGENT_H

#include <stddef.h>

struct msys_x11_agent;

/* Pure native-policy decisions kept exported for the socketpair tests. */
int msys_x11_agent_active_foreground_component(const char *payload,
        char *component, size_t size);
int msys_x11_agent_home_visible(const char *active_component,
        const char *top_role);

/* A missing MSYS_CONTROL_FD is not an error and yields a NULL agent. */
int msys_x11_agent_start(struct msys_x11_agent **agent,
        const char *display_name);

/*
 * Pump the private mIPC channel and display-session file without blocking.
 * Returns 0 while running, 75 when a selected DISPLAY changed, and a positive
 * process exit status on a fatal transport error.
 */
int msys_x11_agent_poll(struct msys_x11_agent *agent);

/*
 * Wake the exact-component transition watcher after an X11 top-level surface
 * maps, unmaps, is destroyed, or publishes identity metadata.  This is only a
 * lifecycle hint: it never paints, captures, or damages the display.
 */
void msys_x11_agent_notify_window_change(struct msys_x11_agent *agent);
void msys_x11_agent_stop(struct msys_x11_agent *agent);

#endif
