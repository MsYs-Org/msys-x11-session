#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "msys_x11_policy_api.h"

#include <X11/Xlib.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int environment_enabled(const char *name, int fallback)
{
    const char *value = getenv(name);

    if (!value || !*value)
        return fallback;
    return strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0 &&
        strcasecmp(value, "no") != 0 && strcasecmp(value, "off") != 0;
}

static int safe_display(const char *value)
{
    const char *cursor = value;

    if (!cursor || *cursor++ != ':' || !isdigit((unsigned char)*cursor))
        return 0;
    while (isdigit((unsigned char)*cursor))
        cursor++;
    if (*cursor == '.') {
        cursor++;
        if (!isdigit((unsigned char)*cursor))
            return 0;
        while (isdigit((unsigned char)*cursor))
            cursor++;
    }
    return *cursor == '\0';
}

static char *quote_json(const char *value)
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

static int parse_matrix(const char *text, double matrix[9])
{
    const char *cursor = text;
    int field;
    double determinant;

    if (!text || !*text)
        return 0;
    for (field = 0; field < 9; field++) {
        char *end = NULL;

        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        errno = 0;
        matrix[field] = strtod(cursor, &end);
        if (errno || !end || end == cursor || !isfinite(matrix[field]) ||
                fabs(matrix[field]) > 1000000.0)
            return 0;
        cursor = end;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (field < 8) {
            if (*cursor != ',' && *cursor != ' ')
                return 0;
            cursor++;
        }
    }
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    if (*cursor)
        return 0;
    determinant =
        matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
        matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
        matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    return fabs(matrix[8]) >= 1e-12 && fabs(determinant) >= 1e-12;
}

static void ch347_matrix(double matrix[9])
{
    int swap = environment_enabled("CH347_TOUCH_SWAP_XY", 0);
    int invert_x = environment_enabled("CH347_TOUCH_INVERT_X", 0);
    int invert_y = environment_enabled("CH347_TOUCH_INVERT_Y", 0);
    double x_scale = invert_x ? -1.0 : 1.0;
    double y_scale = invert_y ? -1.0 : 1.0;
    double x_offset = invert_x ? 1.0 : 0.0;
    double y_offset = invert_y ? 1.0 : 0.0;

    if (swap) {
        double values[9] = {
            0.0, x_scale, x_offset,
            y_scale, 0.0, y_offset,
            0.0, 0.0, 1.0
        };
        memcpy(matrix, values, sizeof(values));
    } else {
        double values[9] = {
            x_scale, 0.0, x_offset,
            0.0, y_scale, y_offset,
            0.0, 0.0, 1.0
        };
        memcpy(matrix, values, sizeof(values));
    }
}

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t count = write(fd, buffer + offset, length - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return 0;
        offset += (size_t)count;
    }
    return 1;
}

static int atomic_write(const char *path, const char *document)
{
    char temporary[PATH_MAX];
    char directory[PATH_MAX];
    char *slash;
    int fd = -1;
    int directory_fd = -1;
    int result = 0;

    if (!path || !*path || strlen(path) >= PATH_MAX - 64u)
        return 0;
    snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", path,
            (long)getpid());
    snprintf(directory, sizeof(directory), "%s", path);
    slash = strrchr(directory, '/');
    if (slash) {
        if (slash == directory)
            slash[1] = '\0';
        else
            *slash = '\0';
    } else {
        memcpy(directory, ".", sizeof("."));
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        goto done;
    if (!write_all(fd, document, strlen(document)) || fsync(fd) != 0 ||
            close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (rename(temporary, path) != 0)
        goto done;
    directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd >= 0 && fsync(directory_fd) != 0)
        goto done;
    result = 1;

done:
    if (fd >= 0)
        close(fd);
    if (directory_fd >= 0)
        close(directory_fd);
    if (!result)
        unlink(temporary);
    return result;
}

int msys_x11_publish_display_session(const char *display_name,
        const char *provider, const char *state_file)
{
    Display *display = NULL;
    XWindowAttributes attributes;
    const char *mode = getenv("MSYS_DISPLAY_INPUT_MODE");
    const char *explicit_matrix = getenv("MSYS_INPUT_TRANSFORM");
    const char *device = getenv("MSYS_INPUT_DEVICE");
    const char *generation_text = getenv("MSYS_GENERATION");
    const char *source;
    double matrix[9];
    int enabled = 0;
    char matrix_json[256] = "null";
    char *quoted_provider = NULL;
    char *quoted_display = NULL;
    char *quoted_mode = NULL;
    char *quoted_device = NULL;
    char *quoted_source = NULL;
    char *document = NULL;
    struct timespec realtime;
    unsigned long generation = 0;
    char *end = NULL;
    int length;
    int result = 1;

    if (!safe_display(display_name) || !provider || !*provider ||
            strlen(provider) > 256 || !state_file || !*state_file)
        goto done;
    if (!mode || !*mode || strcmp(mode, "auto") == 0)
        mode = getenv("CH347_TOUCH") ? "ch347-direct" : "none";
    if (!device || !*device)
        device = strncmp(mode, "ch347-", 7) == 0 ? "CH347 XPT2046" : "";
    if (explicit_matrix && *explicit_matrix) {
        if (strcmp(mode, "none") == 0 || !parse_matrix(explicit_matrix,
                    matrix))
            goto done;
        enabled = 1;
        source = "provider-effective-environment";
    } else if (strcmp(mode, "ch347-direct") == 0 ||
            strcmp(mode, "ch347-xtest") == 0) {
        enabled = environment_enabled("CH347_TOUCH", 1);
        if (enabled)
            ch347_matrix(matrix);
        source = strcmp(mode, "ch347-xtest") == 0
            ? "ch347-xtest-effective" : "ch347-direct-effective";
    } else if (strcmp(mode, "none") == 0) {
        source = "no-provider-owned-input";
    } else {
        /* Native v1 intentionally does not shell out to xinput. */
        goto done;
    }
    if (enabled) {
        length = snprintf(matrix_json, sizeof(matrix_json),
                "[%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g]",
                matrix[0], matrix[1], matrix[2], matrix[3], matrix[4],
                matrix[5], matrix[6], matrix[7], matrix[8]);
        if (length < 0 || (size_t)length >= sizeof(matrix_json))
            goto done;
    }
    display = XOpenDisplay(display_name);
    if (!display || !XGetWindowAttributes(display,
                DefaultRootWindow(display), &attributes))
        goto done;
    if (generation_text && *generation_text) {
        errno = 0;
        generation = strtoul(generation_text, &end, 10);
        if (errno || !end || *end)
            generation = 0;
    }
    if (clock_gettime(CLOCK_REALTIME, &realtime) != 0)
        goto done;
    quoted_provider = quote_json(provider);
    quoted_display = quote_json(display_name);
    quoted_mode = quote_json(mode);
    quoted_device = *device ? quote_json(device) : strdup("null");
    quoted_source = quote_json(source);
    if (!quoted_provider || !quoted_display || !quoted_mode ||
            !quoted_device || !quoted_source)
        goto done;
    length = snprintf(NULL, 0,
            "{\"schema\":\"msys.display-session.v1\",\"state\":\"ready\","
            "\"provider\":%s,\"generation\":%lu,\"display\":%s,"
            "\"geometry\":{\"width\":%d,\"height\":%d,\"depth\":%d},"
            "\"input_transform\":{\"enabled\":%s,\"mode\":%s,"
            "\"device\":%s,\"space\":\"normalized-display\","
            "\"matrix\":%s,\"source\":%s,\"verified\":true},"
            "\"observed_at_unix_ms\":%lld}\n",
            quoted_provider, generation, quoted_display,
            attributes.width, attributes.height, attributes.depth,
            enabled ? "true" : "false", quoted_mode, quoted_device,
            matrix_json, quoted_source,
            (long long)realtime.tv_sec * 1000ll + realtime.tv_nsec / 1000000ll);
    if (length < 0)
        goto done;
    document = malloc((size_t)length + 1u);
    if (!document)
        goto done;
    snprintf(document, (size_t)length + 1u,
            "{\"schema\":\"msys.display-session.v1\",\"state\":\"ready\","
            "\"provider\":%s,\"generation\":%lu,\"display\":%s,"
            "\"geometry\":{\"width\":%d,\"height\":%d,\"depth\":%d},"
            "\"input_transform\":{\"enabled\":%s,\"mode\":%s,"
            "\"device\":%s,\"space\":\"normalized-display\","
            "\"matrix\":%s,\"source\":%s,\"verified\":true},"
            "\"observed_at_unix_ms\":%lld}\n",
            quoted_provider, generation, quoted_display,
            attributes.width, attributes.height, attributes.depth,
            enabled ? "true" : "false", quoted_mode, quoted_device,
            matrix_json, quoted_source,
            (long long)realtime.tv_sec * 1000ll + realtime.tv_nsec / 1000000ll);
    if (!atomic_write(state_file, document))
        goto done;
    fputs(document, stdout);
    result = 0;

done:
    if (display)
        XCloseDisplay(display);
    free(quoted_provider);
    free(quoted_display);
    free(quoted_mode);
    free(quoted_device);
    free(quoted_source);
    free(document);
    if (result != 0)
        fprintf(stderr, "msys-display-session: native publish failed\n");
    return result;
}
