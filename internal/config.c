#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UMRK_SSH_MLP1_DEFAULT_INTERNAL_DATA_PATH "/userdata"
#define UMRK_SSH_MLP1_DEFAULT_SDCARD_PATH "/mnt/sdcard"

static void umrk__set_error(char *error, size_t error_len, const char *fmt, ...) {
    va_list args;

    if (!error || error_len == 0 || !fmt) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(error, error_len, fmt, args);
    va_end(args);
}

static int umrk__path_join(char *out, size_t out_len, const char *base, const char *suffix) {
    if (!out || !base || !suffix) {
        return -1;
    }
    return snprintf(out, out_len, "%s/%s", base, suffix) >= (int)out_len ? -1 : 0;
}

static const char *umrk__env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0]) ? value : NULL;
}

static const char *umrk__default_start_dir(void) {
    const char *sdcard = umrk__env_value("SDCARD_PATH");
    if (sdcard) {
        return sdcard;
    }
#ifdef PLATFORM_MLP1
    return UMRK_SSH_MLP1_DEFAULT_SDCARD_PATH;
#else
    return "/";
#endif
}

static int umrk__parse_port_value(const char *text, int *port_out, char *error, size_t error_len) {
    char *end = NULL;
    long parsed;

    if (!text || text[0] == '\0') {
        umrk__set_error(error, error_len, "%s", "port is required");
        return -1;
    }
    if (strchr(text, ' ') != NULL || strchr(text, '\t') != NULL) {
        umrk__set_error(error, error_len, "%s", "port cannot contain spaces");
        return -1;
    }

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        umrk__set_error(error, error_len, "%s", "port must be numeric");
        return -1;
    }
    if (parsed < 1 || parsed > 65535) {
        umrk__set_error(error, error_len, "%s", "port must be between 1 and 65535");
        return -1;
    }

    if (port_out) {
        *port_out = (int)parsed;
    }
    return 0;
}

int umrk_ssh_parse_port_text(const char *text, int *port_out, char *error, size_t error_len) {
    return umrk__parse_port_value(text, port_out, error, error_len);
}

int umrk_ssh_config_get_port(const umrk_ssh_config *cfg, int *port_out, char *error, size_t error_len) {
    const char *colon;

    if (!cfg) {
        umrk__set_error(error, error_len, "%s", "missing config");
        return -1;
    }

    colon = strrchr(cfg->bind_address, ':');
    if (!colon || colon[1] == '\0') {
        umrk__set_error(error, error_len, "%s", "saved ip:port is invalid");
        return -1;
    }

    return umrk__parse_port_value(colon + 1, port_out, error, error_len);
}

int umrk_ssh_config_set_port(umrk_ssh_config *cfg, int port, char *error, size_t error_len) {
    if (!cfg) {
        umrk__set_error(error, error_len, "%s", "missing config");
        return -1;
    }
    if (port < 1 || port > 65535) {
        umrk__set_error(error, error_len, "%s", "port must be between 1 and 65535");
        return -1;
    }
    if (snprintf(cfg->bind_address, sizeof(cfg->bind_address), "0.0.0.0:%d", port) >= (int)sizeof(cfg->bind_address)) {
        umrk__set_error(error, error_len, "%s", "bind address is too long");
        return -1;
    }
    return 0;
}

static void umrk__trim_line(char *line) {
    size_t len;

    if (!line) {
        return;
    }

    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

static int umrk__escape_value(const char *value, char *out, size_t out_len) {
    size_t i;
    size_t used = 0;

    if (!value || !out || out_len == 0) {
        return -1;
    }

    for (i = 0; value[i] != '\0'; ++i) {
        const char *rep = NULL;
        char literal[2] = { value[i], '\0' };

        switch (value[i]) {
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            case '=':  rep = "\\="; break;
            default:   rep = literal; break;
        }

        while (*rep) {
            if (used + 1 >= out_len) {
                return -1;
            }
            out[used++] = *rep++;
        }
    }

    out[used] = '\0';
    return 0;
}

static void umrk__unescape_value(char *value) {
    size_t src = 0;
    size_t dst = 0;

    if (!value) {
        return;
    }

    while (value[src] != '\0') {
        if (value[src] == '\\' && value[src + 1] != '\0') {
            src++;
            switch (value[src]) {
                case 'n': value[dst++] = '\n'; break;
                case 'r': value[dst++] = '\r'; break;
                case 't': value[dst++] = '\t'; break;
                case '=': value[dst++] = '='; break;
                case '\\': value[dst++] = '\\'; break;
                default:
                    value[dst++] = value[src];
                    break;
            }
            src++;
            continue;
        }
        value[dst++] = value[src++];
    }
    value[dst] = '\0';
}

int umrk_ssh_ensure_dir(const char *path, mode_t mode, char *error, size_t error_len) {
    char partial[PATH_MAX];
    size_t len;
    size_t i;

    if (!path || path[0] == '\0') {
        umrk__set_error(error, error_len, "%s", "missing directory path");
        return -1;
    }

    if (snprintf(partial, sizeof(partial), "%s", path) >= (int)sizeof(partial)) {
        umrk__set_error(error, error_len, "%s", "directory path too long");
        return -1;
    }

    len = strlen(partial);
    if (len == 0) {
        umrk__set_error(error, error_len, "%s", "empty directory path");
        return -1;
    }

    if (partial[len - 1] == '/' && len > 1) {
        partial[len - 1] = '\0';
    }

    for (i = 1; partial[i] != '\0'; ++i) {
        if (partial[i] != '/') {
            continue;
        }
        partial[i] = '\0';
        if (mkdir(partial, mode) != 0 && errno != EEXIST) {
            umrk__set_error(error, error_len, "mkdir %s failed: %s", partial, strerror(errno));
            return -1;
        }
        partial[i] = '/';
    }

    if (mkdir(partial, mode) != 0 && errno != EEXIST) {
        umrk__set_error(error, error_len, "mkdir %s failed: %s", partial, strerror(errno));
        return -1;
    }

    return 0;
}

void umrk_ssh_config_set_defaults(umrk_ssh_config *cfg) {
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->username, sizeof(cfg->username), "%s", UMRK_SSH_DEFAULT_USERNAME);
    snprintf(cfg->bind_address, sizeof(cfg->bind_address), "%s", UMRK_SSH_DEFAULT_BIND);
    snprintf(cfg->start_dir, sizeof(cfg->start_dir), "%s", umrk__default_start_dir());
}

int umrk_ssh_paths_init(umrk_ssh_paths *paths, char *error, size_t error_len) {
    const char *state_root = umrk__env_value("UMRK_SSH_STATE_DIR");
    const char *app_root = umrk__env_value("UMRK_SSH_APP_ROOT");

    if (!paths) {
        umrk__set_error(error, error_len, "%s", "missing paths struct");
        return -1;
    }

    memset(paths, 0, sizeof(*paths));

    if (app_root && app_root[0]) {
        if (snprintf(paths->app_root, sizeof(paths->app_root), "%s", app_root) >= (int)sizeof(paths->app_root)) {
            umrk__set_error(error, error_len, "%s", "app root path too long");
            return -1;
        }
    } else if (!getcwd(paths->app_root, sizeof(paths->app_root))) {
        umrk__set_error(error, error_len, "getcwd failed: %s", strerror(errno));
        return -1;
    }

    if (state_root && state_root[0]) {
        if (snprintf(paths->state_root, sizeof(paths->state_root), "%s", state_root) >= (int)sizeof(paths->state_root)) {
            umrk__set_error(error, error_len, "%s", "state root path too long");
            return -1;
        }
    } else {
#ifdef PLATFORM_MLP1
        const char *state_base = umrk__env_value("UMRK_INTERNAL_DATA_PATH");
        if (!state_base) {
            state_base = umrk__env_value("USERDATA_PATH");
        }
        if (!state_base) {
            state_base = UMRK_SSH_MLP1_DEFAULT_INTERNAL_DATA_PATH;
        }
        if (umrk__path_join(paths->state_root, sizeof(paths->state_root),
                            state_base, "umrk-ssh-server") != 0) {
            umrk__set_error(error, error_len, "%s", "state root path too long");
            return -1;
        }
#else
        if (snprintf(paths->state_root, sizeof(paths->state_root), "%s/build/runtime", paths->app_root) >= (int)sizeof(paths->state_root)) {
            umrk__set_error(error, error_len, "%s", "desktop state root path too long");
            return -1;
        }
#endif
    }

    if (umrk__path_join(paths->config_path, sizeof(paths->config_path), paths->state_root, "config.ini") != 0 ||
        umrk__path_join(paths->hostkeys_dir, sizeof(paths->hostkeys_dir), paths->state_root, "hostkeys") != 0 ||
        umrk__path_join(paths->run_dir, sizeof(paths->run_dir), paths->state_root, "run") != 0 ||
        umrk__path_join(paths->log_dir, sizeof(paths->log_dir), paths->state_root, "logs") != 0 ||
        umrk__path_join(paths->app_log_path, sizeof(paths->app_log_path), paths->log_dir, "ssh-server.txt") != 0 ||
        umrk__path_join(paths->backups_dir, sizeof(paths->backups_dir), paths->state_root, "backups") != 0 ||
        umrk__path_join(paths->bundled_dropbear, sizeof(paths->bundled_dropbear), paths->app_root, "runtime/bin/dropbear") != 0 ||
        umrk__path_join(paths->bundled_dropbearkey, sizeof(paths->bundled_dropbearkey), paths->app_root, "runtime/bin/dropbearkey") != 0) {
        umrk__set_error(error, error_len, "%s", "derived path too long");
        return -1;
    }

    return 0;
}

int umrk_ssh_config_load(const umrk_ssh_paths *paths, umrk_ssh_config *cfg,
                         char *error, size_t error_len) {
    FILE *fp;
    char line[4096];

    if (!paths || !cfg) {
        umrk__set_error(error, error_len, "%s", "missing config load input");
        return -1;
    }

    umrk_ssh_config_set_defaults(cfg);

    fp = fopen(paths->config_path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            return 0;
        }
        umrk__set_error(error, error_len, "open %s failed: %s", paths->config_path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *sep;
        char *key;
        char *value;

        umrk__trim_line(line);
        key = line;
        while (*key && isspace((unsigned char)*key)) {
            key++;
        }
        if (*key == '\0' || *key == '#') {
            continue;
        }

        sep = strchr(key, '=');
        if (!sep) {
            continue;
        }

        *sep = '\0';
        value = sep + 1;
        umrk__unescape_value(value);

        if (strcmp(key, "username") == 0) {
            snprintf(cfg->username, sizeof(cfg->username), "%s", value);
        } else if (strcmp(key, "password") == 0) {
            snprintf(cfg->password, sizeof(cfg->password), "%s", value);
        } else if (strcmp(key, "bind_address") == 0) {
            snprintf(cfg->bind_address, sizeof(cfg->bind_address), "%s", value);
        } else if (strcmp(key, "port") == 0) {
            if (snprintf(cfg->bind_address, sizeof(cfg->bind_address), "0.0.0.0:%d", atoi(value)) >= (int)sizeof(cfg->bind_address)) {
                snprintf(cfg->bind_address, sizeof(cfg->bind_address), "%s", UMRK_SSH_DEFAULT_BIND);
            }
        } else if (strcmp(key, "start_dir") == 0) {
            snprintf(cfg->start_dir, sizeof(cfg->start_dir), "%s", value);
        } else if (strcmp(key, "last_applied_username") == 0) {
            snprintf(cfg->last_applied_username, sizeof(cfg->last_applied_username), "%s", value);
        }
    }

    fclose(fp);
    return 0;
}

int umrk_ssh_config_save(const umrk_ssh_paths *paths, const umrk_ssh_config *cfg,
                         char *error, size_t error_len) {
    FILE *fp;
    char escaped[PATH_MAX * 2];
    char tmp_path[PATH_MAX];

    if (!paths || !cfg) {
        umrk__set_error(error, error_len, "%s", "missing config save input");
        return -1;
    }

    if (umrk_ssh_ensure_dir(paths->state_root, 0755, error, error_len) != 0) {
        return -1;
    }

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", paths->config_path) >= (int)sizeof(tmp_path)) {
        umrk__set_error(error, error_len, "%s", "config temp path too long");
        return -1;
    }

    fp = fopen(tmp_path, "w");
    if (!fp) {
        umrk__set_error(error, error_len, "open %s failed: %s", tmp_path, strerror(errno));
        return -1;
    }

    if (umrk__escape_value(cfg->username, escaped, sizeof(escaped)) != 0 ||
        fprintf(fp, "username=%s\n", escaped) < 0 ||
        umrk__escape_value(cfg->password, escaped, sizeof(escaped)) != 0 ||
        fprintf(fp, "password=%s\n", escaped) < 0 ||
        umrk__escape_value(cfg->bind_address, escaped, sizeof(escaped)) != 0 ||
        fprintf(fp, "bind_address=%s\n", escaped) < 0 ||
        umrk__escape_value(cfg->start_dir, escaped, sizeof(escaped)) != 0 ||
        fprintf(fp, "start_dir=%s\n", escaped) < 0 ||
        umrk__escape_value(cfg->last_applied_username, escaped, sizeof(escaped)) != 0 ||
        fprintf(fp, "last_applied_username=%s\n", escaped) < 0) {
        fclose(fp);
        unlink(tmp_path);
        umrk__set_error(error, error_len, "%s", "writing config failed");
        return -1;
    }

    if (fclose(fp) != 0) {
        unlink(tmp_path);
        umrk__set_error(error, error_len, "closing %s failed: %s", tmp_path, strerror(errno));
        return -1;
    }

    if (rename(tmp_path, paths->config_path) != 0) {
        unlink(tmp_path);
        umrk__set_error(error, error_len, "rename %s -> %s failed: %s", tmp_path, paths->config_path, strerror(errno));
        return -1;
    }

    return 0;
}
