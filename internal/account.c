#include "account.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char *crypt(const char *key, const char *salt);

static void umrk__set_error(char *error, size_t error_len, const char *fmt, ...) {
    va_list args;

    if (!error || error_len == 0 || !fmt) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(error, error_len, fmt, args);
    va_end(args);
}

static int umrk__copy_file(const char *src, const char *dst, mode_t mode,
                           char *error, size_t error_len) {
    FILE *in = NULL;
    FILE *out = NULL;
    char buffer[4096];
    size_t bytes;

    in = fopen(src, "rb");
    if (!in) {
        umrk__set_error(error, error_len, "open %s failed: %s", src, strerror(errno));
        return -1;
    }

    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        umrk__set_error(error, error_len, "open %s failed: %s", dst, strerror(errno));
        return -1;
    }

    while ((bytes = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, bytes, out) != bytes) {
            fclose(in);
            fclose(out);
            unlink(dst);
            umrk__set_error(error, error_len, "write %s failed: %s", dst, strerror(errno));
            return -1;
        }
    }

    if (ferror(in)) {
        fclose(in);
        fclose(out);
        unlink(dst);
        umrk__set_error(error, error_len, "read %s failed: %s", src, strerror(errno));
        return -1;
    }

    fclose(in);
    if (fclose(out) != 0) {
        unlink(dst);
        umrk__set_error(error, error_len, "close %s failed: %s", dst, strerror(errno));
        return -1;
    }

    if (chmod(dst, mode) != 0) {
        umrk__set_error(error, error_len, "chmod %s failed: %s", dst, strerror(errno));
        return -1;
    }

    return 0;
}

static int umrk__validate_username(const char *username, char *error, size_t error_len) {
    size_t i;

    if (!username || username[0] == '\0') {
        umrk__set_error(error, error_len, "%s", "username is required");
        return -1;
    }
    if (!isalpha((unsigned char)username[0]) && username[0] != '_') {
        umrk__set_error(error, error_len, "%s", "username must start with a letter or underscore");
        return -1;
    }

    for (i = 0; username[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)username[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            umrk__set_error(error, error_len, "%s", "username allows only letters, digits, _ and -");
            return -1;
        }
    }

    return 0;
}

static int umrk__validate_password(const char *password, char *error, size_t error_len) {
    if (!password || password[0] == '\0') {
        umrk__set_error(error, error_len, "%s", "password is required");
        return -1;
    }
    if (strchr(password, ':') != NULL) {
        umrk__set_error(error, error_len, "%s", "password cannot contain ':'");
        return -1;
    }
    if (strchr(password, '\n') != NULL || strchr(password, '\r') != NULL) {
        umrk__set_error(error, error_len, "%s", "password cannot contain newlines");
        return -1;
    }
    return 0;
}

static int umrk__validate_start_dir(const char *path, char *error, size_t error_len) {
    if (!path || path[0] == '\0') {
        umrk__set_error(error, error_len, "%s", "start folder is required");
        return -1;
    }
    if (path[0] != '/') {
        umrk__set_error(error, error_len, "%s", "start folder must be an absolute path");
        return -1;
    }
    return 0;
}

int umrk_ssh_validate_bind_address(const char *text, char *error, size_t error_len) {
    const char *colon;
    char *end = NULL;
    long parsed;

    if (!text || text[0] == '\0') {
        umrk__set_error(error, error_len, "%s", "ip:port is required");
        return -1;
    }
    if (strchr(text, ' ') != NULL || strchr(text, '\t') != NULL) {
        umrk__set_error(error, error_len, "%s", "ip:port cannot contain spaces");
        return -1;
    }

    colon = strrchr(text, ':');
    if (!colon || colon == text || colon[1] == '\0') {
        umrk__set_error(error, error_len, "%s", "use ip:port");
        return -1;
    }

    errno = 0;
    parsed = strtol(colon + 1, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        umrk__set_error(error, error_len, "%s", "port must be numeric");
        return -1;
    }
    if (parsed < 1 || parsed > 65535) {
        umrk__set_error(error, error_len, "%s", "port must be between 1 and 65535");
        return -1;
    }

    return 0;
}

static int umrk__read_random_bytes(unsigned char *buf, size_t len) {
    int fd;
    ssize_t got;
    size_t total = 0;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    while (total < len) {
        got = read(fd, buf + total, len - total);
        if (got <= 0) {
            close(fd);
            return -1;
        }
        total += (size_t)got;
    }

    close(fd);
    return 0;
}

static int umrk__password_hash(const char *password, char *out, size_t out_len,
                               char *error, size_t error_len) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    unsigned char random_bytes[16];
    char salt[64];
    char *hashed;
    size_t i;

    if (umrk__read_random_bytes(random_bytes, sizeof(random_bytes)) != 0) {
        umrk__set_error(error, error_len, "%s", "failed to read /dev/urandom");
        return -1;
    }

    memcpy(salt, "$6$", 3);
    for (i = 0; i < sizeof(random_bytes); ++i) {
        salt[3 + i] = alphabet[random_bytes[i] % (sizeof(alphabet) - 1)];
    }
    salt[3 + sizeof(random_bytes)] = '$';
    salt[4 + sizeof(random_bytes)] = '\0';

    hashed = crypt(password, salt);
    if (!hashed || hashed[0] == '\0') {
        umrk__set_error(error, error_len, "%s", "crypt() failed");
        return -1;
    }

    if (snprintf(out, out_len, "%s", hashed) >= (int)out_len) {
        umrk__set_error(error, error_len, "%s", "password hash too long");
        return -1;
    }

    return 0;
}

static long umrk__shadow_day_count(void) {
    time_t now = time(NULL);
    if (now <= 0) {
        return 0;
    }
    return (long)(now / 86400);
}

static int umrk__rewrite_passwd(const umrk_ssh_config *cfg, const char *stale_username,
                                char *error, size_t error_len) {
    FILE *in = NULL;
    FILE *out = NULL;
    char line[4096];
    bool found = false;
    char temp_path[] = "/etc/passwd.umrk.tmp";

    in = fopen("/etc/passwd", "r");
    if (!in) {
        umrk__set_error(error, error_len, "open /etc/passwd failed: %s", strerror(errno));
        return -1;
    }
    out = fopen(temp_path, "w");
    if (!out) {
        fclose(in);
        umrk__set_error(error, error_len, "open %s failed: %s", temp_path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, cfg->username, strlen(cfg->username)) == 0 &&
            line[strlen(cfg->username)] == ':') {
            fprintf(out, "%s:x:0:0:UMRK SSH Admin:%s:/bin/bash\n", cfg->username, cfg->start_dir);
            found = true;
            continue;
        }
        if (stale_username && stale_username[0] &&
            strncmp(line, stale_username, strlen(stale_username)) == 0 &&
            line[strlen(stale_username)] == ':') {
            continue;
        }
        fputs(line, out);
    }

    if (!found) {
        fprintf(out, "%s:x:0:0:UMRK SSH Admin:%s:/bin/bash\n", cfg->username, cfg->start_dir);
    }

    fclose(in);
    if (fclose(out) != 0) {
        unlink(temp_path);
        umrk__set_error(error, error_len, "close %s failed: %s", temp_path, strerror(errno));
        return -1;
    }

    if (rename(temp_path, "/etc/passwd") != 0 || chmod("/etc/passwd", 0644) != 0) {
        unlink(temp_path);
        umrk__set_error(error, error_len, "updating /etc/passwd failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int umrk__rewrite_shadow(const umrk_ssh_config *cfg, const char *stale_username,
                                const char *hash, char *error, size_t error_len) {
    FILE *in = NULL;
    FILE *out = NULL;
    char line[4096];
    bool found = false;
    char temp_path[] = "/etc/shadow.umrk.tmp";
    long day_count = umrk__shadow_day_count();

    in = fopen("/etc/shadow", "r");
    if (!in) {
        umrk__set_error(error, error_len, "open /etc/shadow failed: %s", strerror(errno));
        return -1;
    }
    out = fopen(temp_path, "w");
    if (!out) {
        fclose(in);
        umrk__set_error(error, error_len, "open %s failed: %s", temp_path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, cfg->username, strlen(cfg->username)) == 0 &&
            line[strlen(cfg->username)] == ':') {
            fprintf(out, "%s:%s:%ld:0:99999:7:::\n", cfg->username, hash, day_count);
            found = true;
            continue;
        }
        if (stale_username && stale_username[0] &&
            strncmp(line, stale_username, strlen(stale_username)) == 0 &&
            line[strlen(stale_username)] == ':') {
            continue;
        }
        fputs(line, out);
    }

    if (!found) {
        fprintf(out, "%s:%s:%ld:0:99999:7:::\n", cfg->username, hash, day_count);
    }

    fclose(in);
    if (fclose(out) != 0) {
        unlink(temp_path);
        umrk__set_error(error, error_len, "close %s failed: %s", temp_path, strerror(errno));
        return -1;
    }

    if (rename(temp_path, "/etc/shadow") != 0 || chmod("/etc/shadow", 0600) != 0) {
        unlink(temp_path);
        umrk__set_error(error, error_len, "updating /etc/shadow failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int umrk_ssh_apply_account(const umrk_ssh_config *cfg, const umrk_ssh_paths *paths,
                           char *status, size_t status_len) {
    char hash[512];
    char passwd_backup[PATH_MAX];
    char shadow_backup[PATH_MAX];
    const char *stale_username = NULL;

    if (!cfg || !paths) {
        umrk__set_error(status, status_len, "%s", "missing account apply input");
        return -1;
    }

    if (umrk__validate_username(cfg->username, status, status_len) != 0 ||
        umrk__validate_password(cfg->password, status, status_len) != 0 ||
        umrk__validate_start_dir(cfg->start_dir, status, status_len) != 0) {
        return -1;
    }

    if (umrk_ssh_ensure_dir(paths->state_root, 0755, status, status_len) != 0 ||
        umrk_ssh_ensure_dir(paths->backups_dir, 0755, status, status_len) != 0 ||
        umrk_ssh_ensure_dir(cfg->start_dir, 0755, status, status_len) != 0) {
        return -1;
    }

    if (snprintf(passwd_backup, sizeof(passwd_backup), "%s/passwd.bak", paths->backups_dir) >= (int)sizeof(passwd_backup) ||
        umrk__copy_file("/etc/passwd", passwd_backup, 0644, status, status_len) != 0) {
        return -1;
    }
    if (snprintf(shadow_backup, sizeof(shadow_backup), "%s/shadow.bak", paths->backups_dir) >= (int)sizeof(shadow_backup) ||
        umrk__copy_file("/etc/shadow", shadow_backup, 0600, status, status_len) != 0) {
        return -1;
    }

    if (cfg->last_applied_username[0] && strcmp(cfg->last_applied_username, cfg->username) != 0) {
        stale_username = cfg->last_applied_username;
    }

    if (umrk__password_hash(cfg->password, hash, sizeof(hash), status, status_len) != 0) {
        return -1;
    }

    if (umrk__rewrite_passwd(cfg, stale_username, status, status_len) != 0) {
        return -1;
    }
    if (umrk__rewrite_shadow(cfg, stale_username, hash, status, status_len) != 0) {
        umrk__copy_file(passwd_backup, "/etc/passwd", 0644, status, status_len);
        umrk__copy_file(shadow_backup, "/etc/shadow", 0600, status, status_len);
        return -1;
    }

    snprintf(status, status_len, "Applied account %s as UID 0 alias", cfg->username);
    return 0;
}
