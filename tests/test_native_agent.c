#define _POSIX_C_SOURCE 200809L

#include "msys_x11_agent.h"

#include "msys/mipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        return 1; \
    } \
} while (0)

static int receive(msys_mipc_client *peer, char *packet, const char *type,
        int timeout_ms)
{
    char actual[32];
    int result = msys_mipc_recv_json(peer, packet, MSYS_MIPC_RECV_CAPACITY,
            timeout_ms, NULL);

    if (result != MSYS_MIPC_OK)
        return 0;
    return msys_mipc_json_get_string(packet, "type", actual, sizeof(actual),
            NULL) == MSYS_MIPC_OK && strcmp(actual, type) == 0;
}

static int call_and_expect(struct msys_x11_agent *agent,
        msys_mipc_client *peer, char *packet, uint64_t id,
        const char *method, const char *payload, const char *needle)
{
    char request[1024];
    int attempt;

    snprintf(request, sizeof(request),
            "{\"type\":\"call\",\"id\":%llu,\"method\":\"%s\","
            "\"payload\":%s}", (unsigned long long)id, method, payload);
    if (msys_mipc_send_json(peer, request) != MSYS_MIPC_OK)
        return 0;
    for (attempt = 0; attempt < 100; attempt++) {
        int result = msys_x11_agent_poll(agent);
        if (result != 0)
            return 0;
        if (receive(peer, packet, "return", 20)) {
            if (!strstr(packet, needle))
                fprintf(stderr, "unexpected native reply for %s: %s\n",
                        method, packet);
            return strstr(packet, needle) != NULL;
        }
    }
    return 0;
}

int main(void)
{
    int sockets[2];
    char fd_text[32];
    char *packet;
    msys_mipc_client peer;
    struct msys_x11_agent *agent = NULL;
    char component[128];

    CHECK(msys_x11_agent_active_foreground_component(
                "{\"windows\":[{\"component\":\"org.example.old:main\","
                "\"state\":\"background\"},{\"component\":\"org.example.app:main\","
                "\"state\":\"ready\"}]}", component,
                sizeof(component)) == 1);
    CHECK(strcmp(component, "org.example.app:main") == 0);
    CHECK(msys_x11_agent_home_visible(component, "launcher") == 0);
    CHECK(msys_x11_agent_home_visible("", "launcher") == 1);
    CHECK(msys_x11_agent_active_foreground_component(
                "{\"windows\":[{\"component\":\"org.example.old:main\","
                "\"state\":\"background\"}]}", component,
                sizeof(component)) == 0);
    CHECK(component[0] == '\0');

    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
    CHECK(msys_mipc_client_init(&peer, sockets[0], 1) == MSYS_MIPC_OK);
    snprintf(fd_text, sizeof(fd_text), "%d", sockets[1]);
    CHECK(setenv("MSYS_CONTROL_FD", fd_text, 1) == 0);
    CHECK(setenv("MSYS_COMPONENT_ID",
                "org.msys.x11.session:window-policy", 1) == 0);
    CHECK(setenv("MSYS_GENERATION", "9", 1) == 0);
    CHECK(setenv("MSYS_RUNTIME_DIR", "/definitely/missing/msys-runtime", 1) == 0);

    /* A SOCK_SEQPACKET peer may queue welcome before reading hello. This
     * keeps the test single-threaded while exercising the exact SDK wire. */
    CHECK(msys_mipc_send_json(&peer,
                "{\"type\":\"welcome\",\"component\":\"test\","
                "\"generation\":9}") == MSYS_MIPC_OK);
    CHECK(msys_x11_agent_start(&agent, ":99") == 0);
    CHECK(agent != NULL);
    packet = malloc(MSYS_MIPC_RECV_CAPACITY);
    CHECK(packet != NULL);
    CHECK(receive(&peer, packet, "hello", 1000));
    CHECK(strstr(packet, "window-policy") != NULL);
    CHECK(receive(&peer, packet, "ready", 1000));
    CHECK(receive(&peer, packet, "event", 1000));
    CHECK(strstr(packet, "native-c") != NULL);

    CHECK(call_and_expect(agent, &peer, packet, 1, "get_layout", "{}",
                "msys.layout.effective.v1"));
    CHECK(call_and_expect(agent, &peer, packet, 2, "set_layout",
                "{\"profile\":\"mobile\"}", "navigation_region"));
    CHECK(receive(&peer, packet, "event", 1000));
    CHECK(strstr(packet, "msys.layout.changed.v1") != NULL);
    CHECK(call_and_expect(agent, &peer, packet, 3, "list_windows", "{}",
                "msys.window-list.v1"));
    CHECK(call_and_expect(agent, &peer, packet, 4, "activate_component",
                "{\"identity\":\"org.example.app\",\"title\":\"App\"}",
                "\"ok\":true"));
    CHECK(call_and_expect(agent, &peer, packet, 5, "get_display_session", "{}",
                "display-session-unavailable"));
    CHECK(call_and_expect(agent, &peer, packet, 6, "navigation_action",
                "{\"action\":\"invalid\"}",
                "invalid-navigation-action"));
    CHECK(call_and_expect(agent, &peer, packet, 7, "maximize_window",
                "{\"window_id\":\"msys.x11-window.v1:test:0x42\"}",
                "\"placement\":\"maximized\""));
    CHECK(receive(&peer, packet, "event", 1000));
    CHECK(strstr(packet, "msys.window.action") != NULL);

    CHECK(msys_mipc_send_json(&peer, "{\"type\":\"shutdown\"}") ==
            MSYS_MIPC_OK);
    CHECK(msys_x11_agent_poll(agent) == 100);
    msys_x11_agent_stop(agent);
    msys_mipc_client_close(&peer);
    close(sockets[1]);
    free(packet);
    unsetenv("MSYS_CONTROL_FD");
    puts("native agent socketpair tests passed");
    return 0;
}
