#ifndef MSYS_X11_AGENT_H
#define MSYS_X11_AGENT_H

struct msys_x11_agent;

/* A missing MSYS_CONTROL_FD is not an error and yields a NULL agent. */
int msys_x11_agent_start(struct msys_x11_agent **agent,
        const char *display_name);

/*
 * Pump the private mIPC channel and display-session file without blocking.
 * Returns 0 while running, 75 when a selected DISPLAY changed, and a positive
 * process exit status on a fatal transport error.
 */
int msys_x11_agent_poll(struct msys_x11_agent *agent);
void msys_x11_agent_stop(struct msys_x11_agent *agent);

#endif
