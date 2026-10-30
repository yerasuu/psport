// SPDX-License-Identifier: GPL-2.0-or-later
/* The shadNet account's password and token (kb #1336; decanter's secure credential storage design,
 * 3.4), taken once at start: from a one-use file (PS_SHADNET_SECRETS_FILE, removed once read), from
 * stdin (PS_SHADNET_SECRETS=stdin, read until it closes) or, from launchers older than both, from
 * PS_SHADNET_PASSWORD and PS_SHADNET_TOKEN. Then every one of these variables leaves the
 * environment with its bytes overwritten (ps_host_env_scrub), so that the running game's
 * /proc/<pid>/environ, a core or a minidump no longer carries them, and only the online module gets
 * them (PsNetHost). The file and stdin hold "key=value" lines, password= and token=, each value up
 * to the end of its line; at most SECRETS_MAX bytes. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SECRETS_MAX = 4096 };

static char s_sz_password[SECRETS_MAX], s_sz_token[SECRETS_MAX];

static const char *const s_variables[] = {"PS_SHADNET_PASSWORD", "PS_SHADNET_TOKEN", "PS_SHADNET_SECRETS_FILE",
                                          "PS_SHADNET_SECRETS"};

typedef struct {
    int c_lines, c_unknown;
} SecretsText;

static const char *presence(const char *sz_secret) { return sz_secret[0] ? "given" : "none"; }

/* p_text holds cb_text bytes and room for one more. */
static const char *parse(char *p_text, size_t cb_text, SecretsText *p_out) {
    if (memchr(p_text, 0, cb_text)) return "it holds a NUL byte";
    p_text[cb_text] = 0;
    for (char *sz_line = p_text; sz_line && *sz_line;) {
        char *sz_end = strchr(sz_line, '\n');
        if (sz_end) *sz_end++ = 0;
        const size_t cb_line = strlen(sz_line);
        if (cb_line && sz_line[cb_line - 1] == '\r') sz_line[cb_line - 1] = 0;
        if (*sz_line) {
            ++p_out->c_lines;
            char *sz_value = strchr(sz_line, '=');
            if (sz_value) *sz_value++ = 0;
            char *sz_secret = !sz_value ? NULL
                              : !strcmp(sz_line, "password") ? s_sz_password
                              : !strcmp(sz_line, "token")    ? s_sz_token
                                                             : NULL;
            if (sz_secret) {
                ps_host_wipe(sz_secret, SECRETS_MAX);
                memcpy(sz_secret, sz_value, strlen(sz_value) + 1);
            } else
                ++p_out->c_unknown;
        }
        sz_line = sz_end;
    }
    return NULL;
}

/* The file's bytes up to its end: their count, or minus the host's errno (EFBIG past SECRETS_MAX). */
static int64_t read_all(int h_file, char *p_text) {
    size_t cb_text = 0;
    for (;;) {
        const int64_t cb_read = ps_host_fs_read(h_file, p_text + cb_text, SECRETS_MAX + 1 - cb_text);
        if (cb_read == -EINTR) continue;
        if (cb_read <= 0) return cb_read < 0 ? cb_read : (int64_t)cb_text;
        cb_text += (size_t)cb_read;
        if (cb_text > SECRETS_MAX) return -EFBIG;
    }
}

static int take_text(const char *sz_source, int64_t cb_read, char *p_text) {
    SecretsText text = {0};
    const char *sz_refusal = cb_read == -EFBIG ? "it is longer than 4096 bytes"
                             : cb_read < 0    ? strerror((int)-cb_read)
                                              : parse(p_text, (size_t)cb_read, &text);
    ps_host_wipe(p_text, SECRETS_MAX + 1);
    if (sz_refusal) {
        ps_host_wipe(s_sz_password, sizeof(s_sz_password));
        ps_host_wipe(s_sz_token, sizeof(s_sz_token));
        printf("Online: the shadNet secrets from %s are not used: %s\n", sz_source, sz_refusal);
        return 0;
    }
    printf("Online: shadNet secrets from %s (%d lines: password %s, token %s, %d not understood)\n", sz_source,
           text.c_lines, presence(s_sz_password), presence(s_sz_token), text.c_unknown);
    return 1;
}

/* Never through a link, never anything but a regular file only its user may read; removed whatever
 * else went wrong once it is one, since a launcher writes it for this process alone. */
static int take_file(const char *sz_path) {
    char p_text[SECRETS_MAX + 1];
    PsHostStat named, opened;
    int error = ps_host_fs_stat_link(sz_path, &named);
    if (error || !PS_HOST_MODE_IS_REGULAR(named.mode)) {
        printf("Online: PS_SHADNET_SECRETS_FILE %s is not read and left as it is: %s\n", sz_path,
               error ? strerror(error) : "not a regular file (a link, a folder or a device)");
        return 0;
    }
    int64_t cb_read = 0;
    const char *sz_refusal = NULL;
    const int h_file = ps_host_fs_open(sz_path, PS_HOST_OPEN_READ | PS_HOST_OPEN_NONBLOCK, 0);
    if (h_file < 0) {
        cb_read = h_file;
    } else {
        const int is_owner_only = ps_host_fs_is_owner_only(h_file);
        if (ps_host_fs_stat_file(h_file, &opened) || opened.dev != named.dev || opened.ino != named.ino)
            sz_refusal = "it was replaced between its check and its opening";
        else if (is_owner_only == -ENOSYS)
            sz_refusal = "this host cannot tell who may read it (PS_SHADNET_SECRETS=stdin instead)";
        else if (is_owner_only < 0)
            cb_read = is_owner_only;
        else if (!is_owner_only)
            sz_refusal = "its group or other users may read it, or it is another user's (it must be the player's, "
                         "mode 0600)";
        else
            cb_read = read_all(h_file, p_text);
        (void)ps_host_fs_close(h_file);
    }
    const int remove_error = ps_host_fs_remove(sz_path);
    if (remove_error)
        printf("Online: PS_SHADNET_SECRETS_FILE %s could not be removed (%s): its launcher must remove it\n", sz_path,
               strerror(remove_error));
    else
        printf("Online: PS_SHADNET_SECRETS_FILE %s removed\n", sz_path);
    if (sz_refusal) {
        ps_host_wipe(p_text, sizeof(p_text));
        printf("Online: the shadNet secrets from PS_SHADNET_SECRETS_FILE are not used: %s\n", sz_refusal);
        return 0;
    }
    return take_text("PS_SHADNET_SECRETS_FILE", cb_read, p_text);
}

static int take_stdin(void) {
    char p_text[SECRETS_MAX + 1];
    printf("Online: reading the shadNet secrets from stdin until it closes (PS_SHADNET_SECRETS=stdin)\n");
    return take_text("stdin", read_all(0, p_text), p_text);
}

static int take_variable(const char *sz_name, char *sz_out) {
    const char *sz_value = ps_host_env_get(sz_name);
    if (!sz_value || !sz_value[0]) return 0;
    const size_t cb_value = strlen(sz_value);
    if (cb_value >= SECRETS_MAX) {
        printf("Online: %s is longer than %d bytes: not used\n", sz_name, SECRETS_MAX - 1);
        return 0;
    }
    memcpy(sz_out, sz_value, cb_value + 1);
    return 1;
}

static int is_variable_set(const char *sz_name) {
    const char *sz_value = ps_host_env_get(sz_name);
    return sz_value && sz_value[0];
}

static void wipe_secrets(void) {
    ps_host_wipe(s_sz_password, sizeof(s_sz_password));
    ps_host_wipe(s_sz_token, sizeof(s_sz_token));
}

void runtime_secrets_take(void) {
    const char *sz_file = ps_host_env_get("PS_SHADNET_SECRETS_FILE");
    const char *sz_form = ps_host_env_get("PS_SHADNET_SECRETS");
    const int has_variables = is_variable_set("PS_SHADNET_PASSWORD") || is_variable_set("PS_SHADNET_TOKEN");
    if (!is_variable_set("PS_SHADNET_SECRETS_FILE") && !is_variable_set("PS_SHADNET_SECRETS") && !has_variables) {
        if (is_variable_set("PS_SHADNET_NPID"))
            printf("Online: a shadNet account (PS_SHADNET_NPID) without a password or token: none of "
                   "PS_SHADNET_SECRETS_FILE, PS_SHADNET_SECRETS=stdin or PS_SHADNET_PASSWORD is set\n");
        return;
    }
    if (atexit(wipe_secrets)) printf("Online: atexit failed: the shadNet secrets stay in memory until the end\n");
    int is_taken = 0;
    if (sz_file && sz_file[0]) {
        char sz_path[4096];
        snprintf(sz_path, sizeof(sz_path), "%s", sz_file);
        is_taken = take_file(sz_path);
    }
    if (!is_taken && sz_form && sz_form[0]) {
        if (!strcmp(sz_form, "stdin"))
            is_taken = take_stdin();
        else
            printf("Online: PS_SHADNET_SECRETS=%s is not a form psport knows (stdin): ignored\n", sz_form);
    }
    if (!is_taken && has_variables) {
        take_variable("PS_SHADNET_PASSWORD", s_sz_password);
        take_variable("PS_SHADNET_TOKEN", s_sz_token);
        printf("Online: shadNet secrets from PS_SHADNET_PASSWORD and PS_SHADNET_TOKEN (an older launcher: password "
               "%s, token %s)\n", presence(s_sz_password), presence(s_sz_token));
    } else if (has_variables) {
        printf("Online: PS_SHADNET_PASSWORD or PS_SHADNET_TOKEN also set: ignored\n");
    }
    int c_removed = 0;
    for (size_t i = 0; i < sizeof(s_variables) / sizeof(*s_variables); ++i) {
        int is_set;
        const int error = ps_host_env_scrub(s_variables[i], &is_set);
        c_removed += is_set;
        if (error == ENOSYS && is_set)
            printf("Online: %s left the environment, but the block the process started with could not be "
                   "overwritten: it may still show there\n", s_variables[i]);
        else if (error && error != ENOSYS)
            printf("Online: %s could not be removed from the environment: %s\n", s_variables[i], strerror(error));
    }
    printf("Online: %d shadNet secret variables removed from the environment and overwritten\n", c_removed);
}

const char *runtime_secrets_password(void) { return s_sz_password; }
const char *runtime_secrets_token(void) { return s_sz_token; }

/* The new image finds them on its stdin, a pipe written and closed here, as a launcher would. */
void runtime_secrets_pass_on(void) {
    if (!s_sz_password[0] && !s_sz_token[0]) return;
    char p_text[2 * SECRETS_MAX + 32];
    const int cb_text = snprintf(p_text, sizeof(p_text), "password=%s\ntoken=%s\n", s_sz_password, s_sz_token);
    int error = cb_text > SECRETS_MAX ? EFBIG : 0, h_pipe[2] = {-1, -1};
    if (!error) error = ps_host_fs_pipe(h_pipe);
    for (int off_text = 0; !error && off_text < cb_text;) {
        const int64_t cb_written = ps_host_fs_write(h_pipe[1], p_text + off_text, (size_t)(cb_text - off_text));
        if (cb_written == -EINTR) continue;
        if (cb_written <= 0) error = cb_written < 0 ? (int)-cb_written : EIO;
        else off_text += (int)cb_written;
    }
    ps_host_wipe(p_text, sizeof(p_text));
    if (h_pipe[1] >= 0) (void)ps_host_fs_close(h_pipe[1]);
    if (!error) {
        const int h_stdin = ps_host_fs_duplicate(h_pipe[0], 0);
        if (h_stdin != 0) error = h_stdin < 0 ? -h_stdin : EBADF;
    }
    if (h_pipe[0] >= 0) (void)ps_host_fs_close(h_pipe[0]);
    if (!error) error = ps_host_env_set("PS_SHADNET_SECRETS", "stdin", 1);
    if (error)
        printf("Online: the shadNet secrets cannot be passed on to the new process (%s): it starts without them\n",
               strerror(error));
    else
        printf("Online: shadNet secrets passed on to the new process through its stdin\n");
}
