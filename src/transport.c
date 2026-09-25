#define _POSIX_C_SOURCE 200809L
#include "pux/transport.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) (void)snprintf(error, error_size, "%s", message);
}

static void set_errorf(char *error, size_t error_size, const char *fmt, const char *value)
{
    if (error != NULL && error_size > 0U) (void)snprintf(error, error_size, fmt, value);
}

static int parse_status(const char *text, long *status)
{
    if (text == NULL || status == NULL) return -1;
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || (*end != '\0' && *end != '\n') || value < 100L || value > 599L) return -1;
    *status = value;
    return 0;
}

static int write_max_size(char output[32], size_t max_size)
{
    const int n = snprintf(output, 32U, "%zu", max_size);
    return n < 0 || (size_t)n >= 32U ? -1 : 0;
}

int pux_transport_download(const char *url,
                           const char *output_path,
                           size_t max_size,
                           long *http_status,
                           char *error,
                           size_t error_size)
{
    if (http_status != NULL) *http_status = 0L;
    if (url == NULL || url[0] == '\0' || output_path == NULL || output_path[0] == '\0') {
        set_error(error, error_size, "download URL and output path are required");
        return -1;
    }
    if (strncmp(url, "http://", 7U) != 0 && strncmp(url, "https://", 8U) != 0) {
        set_error(error, error_size, "only HTTP(S) repository URLs are supported");
        return -1;
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        set_error(error, error_size, "cannot create transport pipe");
        return -1;
    }

    char max_size_text[32];
    if (write_max_size(max_size_text, max_size) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        set_error(error, error_size, "download size is too large");
        return -1;
    }

    const char *curl = getenv("PUX_CURL");
    if (curl == NULL || curl[0] == '\0') curl = "curl";

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        set_errorf(error, error_size, "cannot start transport: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        (void)dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        char *const args[] = {
            (char *)curl,
            (char *)"--silent",
            (char *)"--show-error",
            (char *)"--location",
            (char *)"--max-redirs", (char *)"5",
            (char *)"--connect-timeout", (char *)"15",
            (char *)"--max-time", (char *)"120",
            (char *)"--retry", (char *)"2",
            (char *)"--retry-delay", (char *)"1",
            (char *)"--proto", (char *)"=http,https",
            (char *)"--max-filesize", max_size_text,
            (char *)"--output", (char *)output_path,
            (char *)"--write-out", (char *)"%{http_code}",
            (char *)url,
            NULL
        };
        execvp(curl, args);
        _exit(127);
    }

    close(pipe_fds[1]);
    char status_text[64];
    size_t used = 0U;
    while (used + 1U < sizeof(status_text)) {
        const ssize_t n = read(pipe_fds[0], status_text + used, sizeof(status_text) - used - 1U);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(pipe_fds[0]);
    status_text[used] = '\0';

    int wait_status = 0;
    while (waitpid(pid, &wait_status, 0) < 0) {
        if (errno == EINTR) continue;
        set_errorf(error, error_size, "cannot wait for transport: %s", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        if (WIFSIGNALED(wait_status)) {
            (void)snprintf(error, error_size, "curl terminated by signal %d", WTERMSIG(wait_status));
        } else {
            (void)snprintf(error, error_size, "curl failed with exit status %d", WEXITSTATUS(wait_status));
        }
        return -1;
    }

    long status = 0L;
    if (parse_status(status_text, &status) != 0) {
        set_error(error, error_size, "transport returned no valid HTTP status");
        return -1;
    }
    if (http_status != NULL) *http_status = status;
    return 0;
}
