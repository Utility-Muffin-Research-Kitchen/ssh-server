#define _GNU_SOURCE
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

static bool umrk__mode_change_unsupported(int error_number) {
    return error_number == EPERM || error_number == EOPNOTSUPP
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
        || error_number == ENOTSUP
#endif
        ;
}

static int umrk__chmod_if_supported(const char *path, mode_t mode,
                                    char *error, size_t error_len) {
    if (chmod(path, mode) == 0 || umrk__mode_change_unsupported(errno)) {
        return 0;
    }
    umrk__set_error(error, error_len, "chmod %s failed: %s",
                    path, strerror(errno));
    return -1;
}

static int umrk__fchmod_if_supported(int fd, mode_t mode,
                                     char *error, size_t error_len) {
    if (fchmod(fd, mode) == 0 || umrk__mode_change_unsupported(errno)) {
        return 0;
    }
    umrk__set_error(error, error_len, "fchmod failed: %s", strerror(errno));
    return -1;
}

static int umrk__write_all(int fd, const void *buffer, size_t length) {
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return -1;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static const char *umrk__passwd_path(void) {
#ifdef UMRK_SSH_ACCOUNT_TEST
    const char *path = getenv("UMRK_SSH_TEST_PASSWD_PATH");
    if (path && path[0]) {
        return path;
    }
#endif
    return "/etc/passwd";
}

static const char *umrk__shadow_path(void) {
#ifdef UMRK_SSH_ACCOUNT_TEST
    const char *path = getenv("UMRK_SSH_TEST_SHADOW_PATH");
    if (path && path[0]) {
        return path;
    }
#endif
    return "/etc/shadow";
}

static int umrk__ensure_rootfs_writable(char *error, size_t error_len) {
#ifdef PLATFORM_MLP1
    if (system("mount -o remount,rw / >/dev/null 2>&1") == 0 ||
        system("mount -o remount,rw /dev/root / >/dev/null 2>&1") == 0) {
        return 0;
    }
    umrk__set_error(error, error_len, "%s", "root filesystem is read-only; remount rw failed");
    return -1;
#else
    (void)error;
    (void)error_len;
    return 0;
#endif
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
    if (strlen(password) < UMRK_SSH_MIN_PASSWORD_LENGTH) {
        umrk__set_error(error, error_len, "password must contain at least %d characters",
                        UMRK_SSH_MIN_PASSWORD_LENGTH);
        return -1;
    }
    return 0;
}

static void umrk__wipe(void *memory, size_t length) {
    volatile unsigned char *cursor = memory;
    while (length > 0) {
        *cursor++ = 0;
        length--;
    }
}

static int umrk__validate_password_hash(const char *hash, char *error, size_t error_len) {
    size_t length;

    if (!hash || hash[0] != '$') {
        umrk__set_error(error, error_len, "%s", "password hash is missing or invalid");
        return -1;
    }
    length = strlen(hash);
    if (length < 20 || length >= 512 || strchr(hash, ':') ||
        strchr(hash, '\n') || strchr(hash, '\r')) {
        umrk__set_error(error, error_len, "%s", "password hash is malformed");
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

static int umrk__install_authorized_keys(const umrk_ssh_config *cfg,
                                         const umrk_ssh_paths *paths,
                                         char *error, size_t error_len) {
    char ssh_dir[PATH_MAX];
    char temp_name[96];
    char buffer[4096];
    struct stat st;
    int source_fd = -1;
    int directory_fd = -1;
    int temp_fd = -1;
    int rc = -1;

    source_fd = open(paths->authorized_keys_path,
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0) {
        if (errno == ENOENT && cfg->password_auth_enabled) {
            return 0;
        }
        umrk__set_error(error, error_len, "%s",
                        errno == ENOENT
                            ? "key-only mode requires a non-empty authorized_keys file"
                            : "could not inspect authorized_keys");
        return -1;
    }
    if (fstat(source_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(source_fd);
        umrk__set_error(error, error_len, "%s", "authorized_keys must be a non-empty regular file");
        return -1;
    }
    if (snprintf(ssh_dir, sizeof(ssh_dir), "%s/.ssh", cfg->start_dir) >=
        (int)sizeof(ssh_dir)) {
        close(source_fd);
        umrk__set_error(error, error_len, "%s", "authorized_keys destination is too long");
        return -1;
    }

    if (lstat(ssh_dir, &st) != 0) {
        if (errno != ENOENT || mkdir(ssh_dir, 0700) != 0) {
            close(source_fd);
            umrk__set_error(error, error_len, "create %s failed: %s",
                            ssh_dir, strerror(errno));
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        close(source_fd);
        umrk__set_error(error, error_len, "%s", ".ssh must be a real directory");
        return -1;
    }
    if (umrk__chmod_if_supported(ssh_dir, 0700, error, error_len) != 0) {
        close(source_fd);
        return -1;
    }

    directory_fd = open(ssh_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        close(source_fd);
        umrk__set_error(error, error_len, "open %s failed: %s",
                        ssh_dir, strerror(errno));
        return -1;
    }
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        snprintf(temp_name, sizeof(temp_name), ".authorized_keys.umrk.%ld.%u",
                 (long)getpid(), attempt);
        temp_fd = openat(directory_fd, temp_name,
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                         0600);
        if (temp_fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (temp_fd < 0) {
        umrk__set_error(error, error_len, "create authorized_keys temp failed: %s",
                        strerror(errno));
        goto done;
    }

    while (true) {
        ssize_t amount = read(source_fd, buffer, sizeof(buffer));
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount < 0 ||
            (amount > 0 && umrk__write_all(temp_fd, buffer, (size_t)amount) != 0)) {
            umrk__set_error(error, error_len, "%s", "copy authorized_keys failed");
            goto done;
        }
        if (amount == 0) {
            break;
        }
    }
    if (fsync(temp_fd) != 0) {
        umrk__set_error(error, error_len, "sync authorized_keys failed: %s",
                        strerror(errno));
        goto done;
    }
    if (umrk__fchmod_if_supported(temp_fd, 0600, error, error_len) != 0) {
        goto done;
    }
    if (close(temp_fd) != 0) {
        temp_fd = -1;
        umrk__set_error(error, error_len, "%s", "close authorized_keys temp failed");
        goto done;
    }
    temp_fd = -1;
    if (renameat(directory_fd, temp_name,
                 directory_fd, "authorized_keys") != 0 ||
        fsync(directory_fd) != 0) {
        umrk__set_error(error, error_len, "publish authorized_keys failed: %s",
                        strerror(errno));
        goto done;
    }
    temp_name[0] = '\0';
    rc = 0;

done:
    if (temp_fd >= 0) {
        close(temp_fd);
    }
    if (directory_fd >= 0 && temp_name[0]) {
        (void)unlinkat(directory_fd, temp_name, 0);
    }
    if (directory_fd >= 0) {
        close(directory_fd);
    }
    close(source_fd);
    return rc;
}

static int umrk__remove_legacy_persistent_backups(const umrk_ssh_paths *paths,
                                                   char *error, size_t error_len) {
    char directory[PATH_MAX];
    const char *names[] = {"passwd.bak", "shadow.bak"};
    struct stat st;
    int directory_fd = -1;
    int state_fd = -1;

    if (snprintf(directory, sizeof(directory), "%s/backups", paths->state_root) >=
        (int)sizeof(directory)) {
        umrk__set_error(error, error_len, "%s", "legacy backup path is too long");
        return -1;
    }
    if (lstat(directory, &st) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        umrk__set_error(error, error_len, "inspect %s failed: %s",
                        directory, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        umrk__set_error(error, error_len, "%s", "legacy backup path is not a real directory");
        return -1;
    }
    directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        umrk__set_error(error, error_len, "open %s failed: %s",
                        directory, strerror(errno));
        return -1;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (unlinkat(directory_fd, names[i], 0) != 0 && errno != ENOENT) {
            close(directory_fd);
            umrk__set_error(error, error_len, "%s", "could not remove legacy credential backup");
            return -1;
        }
    }
    if (fsync(directory_fd) != 0) {
        int saved_errno = errno;
        close(directory_fd);
        umrk__set_error(error, error_len, "fsync %s failed: %s",
                        directory, strerror(saved_errno));
        return -1;
    }
    close(directory_fd);
    if (rmdir(directory) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        umrk__set_error(error, error_len, "%s", "could not remove legacy backup directory");
        return -1;
    }
    state_fd = open(paths->state_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (state_fd < 0 || fsync(state_fd) != 0) {
        int saved_errno = errno;
        if (state_fd >= 0) {
            close(state_fd);
        }
        umrk__set_error(error, error_len, "fsync after credential cleanup failed: %s",
                        strerror(saved_errno));
        return -1;
    }
    close(state_fd);
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

int umrk_ssh_configure_password(umrk_ssh_config *cfg, const char *password,
                                char *status, size_t status_len) {
    char hash[sizeof(cfg->password_hash)];

    if (!cfg || umrk__validate_password(password, status, status_len) != 0) {
        return -1;
    }
    memset(hash, 0, sizeof(hash));
    if (umrk__password_hash(password, hash, sizeof(hash), status, status_len) != 0) {
        return -1;
    }
    snprintf(cfg->password_hash, sizeof(cfg->password_hash), "%s", hash);
    cfg->password_configured = true;
    cfg->password_auth_enabled = true;
    umrk__wipe(cfg->legacy_password, sizeof(cfg->legacy_password));
    umrk__wipe(hash, sizeof(hash));
    return 0;
}

int umrk_ssh_migrate_legacy_password(const umrk_ssh_paths *paths,
                                     umrk_ssh_config *cfg,
                                     char *status, size_t status_len) {
    umrk_ssh_config updated;
    char validation_error[128] = {0};

    if (!paths || !cfg) {
        umrk__set_error(status, status_len, "%s", "missing password migration input");
        return -1;
    }
    if (!cfg->legacy_password[0]) {
        return 0;
    }

    updated = *cfg;
    if (umrk__validate_password(cfg->legacy_password,
                                validation_error,
                                sizeof(validation_error)) != 0) {
        /* Never retain a weak legacy plaintext just because it predates the
           new password floor. Remove it atomically and require the user to set
           a new password before this UID-0 service can start. */
        umrk__wipe(updated.legacy_password, sizeof(updated.legacy_password));
        umrk__wipe(updated.password_hash, sizeof(updated.password_hash));
        updated.password_configured = false;
        updated.password_auth_enabled = true;
        if (umrk_ssh_config_save(paths, &updated, status, status_len) != 0) {
            umrk__wipe(&updated, sizeof(updated));
            return -1;
        }
        umrk__wipe(cfg->legacy_password, sizeof(cfg->legacy_password));
        *cfg = updated;
        umrk__wipe(&updated, sizeof(updated));
        snprintf(status, status_len,
                 "Removed legacy plaintext; set a password of at least %d characters",
                 UMRK_SSH_MIN_PASSWORD_LENGTH);
        return 0;
    }
    if (umrk_ssh_configure_password(&updated, cfg->legacy_password,
                                    status, status_len) != 0 ||
        umrk_ssh_config_save(paths, &updated, status, status_len) != 0) {
        umrk__wipe(&updated, sizeof(updated));
        return -1;
    }
    umrk__wipe(cfg->legacy_password, sizeof(cfg->legacy_password));
    *cfg = updated;
    umrk__wipe(&updated, sizeof(updated));
    snprintf(status, status_len, "%s", "Migrated password to hash-only storage");
    return 0;
}

static long umrk__shadow_day_count(void) {
    time_t now = time(NULL);
    if (now <= 0) {
        return 0;
    }
    return (long)(now / 86400);
}

static bool umrk__line_has_name(const char *line, const char *name) {
    size_t length = name ? strlen(name) : 0;
    return line && name && length > 0 &&
           strncmp(line, name, length) == 0 && line[length] == ':';
}

static int umrk__open_text_input(const char *path, FILE **out,
                                 char *error, size_t error_len) {
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        int saved_errno = errno;
        if (fd >= 0) {
            close(fd);
        }
        umrk__set_error(error, error_len, "open %s failed: %s",
                        path, strerror(saved_errno));
        return -1;
    }
    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        int saved_errno = errno;
        close(fd);
        umrk__set_error(error, error_len, "fdopen %s failed: %s",
                        path, strerror(saved_errno));
        return -1;
    }
    *out = fp;
    return 0;
}

static int umrk__sync_parent_directory(const char *path,
                                       char *error, size_t error_len) {
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent)) {
        umrk__set_error(error, error_len, "%s", "account path is too long");
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) {
        if (slash == parent) {
            slash[1] = '\0';
        } else {
            snprintf(parent, sizeof(parent), "%s", ".");
        }
    } else {
        *slash = '\0';
    }
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fsync(fd) != 0) {
        int saved_errno = errno;
        if (fd >= 0) {
            close(fd);
        }
        umrk__set_error(error, error_len, "fsync %s failed: %s",
                        parent, strerror(saved_errno));
        return -1;
    }
    close(fd);
    return 0;
}

static int umrk__finish_account_file(FILE *input, FILE *output,
                                     int output_fd, const char *temp_path,
                                     const char *destination, mode_t mode,
                                     char *error, size_t error_len) {
    bool input_error = ferror(input) != 0;
    int input_close_rc = fclose(input);
    if (input_error || input_close_rc != 0) {
        fclose(output);
        unlink(temp_path);
        umrk__set_error(error, error_len, "read %s failed", destination);
        return -1;
    }
    if (fflush(output) != 0 || fsync(output_fd) != 0) {
        int saved_errno = errno;
        fclose(output);
        unlink(temp_path);
        umrk__set_error(error, error_len, "sync %s failed: %s",
                        temp_path, strerror(saved_errno));
        return -1;
    }
    if (umrk__fchmod_if_supported(output_fd, mode, error, error_len) != 0) {
        fclose(output);
        unlink(temp_path);
        return -1;
    }
    if (fclose(output) != 0) {
        unlink(temp_path);
        umrk__set_error(error, error_len, "close %s failed: %s",
                        temp_path, strerror(errno));
        return -1;
    }
    if (rename(temp_path, destination) != 0) {
        int saved_errno = errno;
        unlink(temp_path);
        umrk__set_error(error, error_len, "publish %s failed: %s",
                        destination, strerror(saved_errno));
        return -1;
    }
    if (umrk__chmod_if_supported(destination, mode, error, error_len) != 0 ||
        umrk__sync_parent_directory(destination, error, error_len) != 0) {
        return -1;
    }
    return 0;
}

static int umrk__open_account_temp(const char *destination, mode_t mode,
                                   char *temp_path, size_t temp_path_size,
                                   FILE **out, int *fd_out,
                                   char *error, size_t error_len) {
    if (snprintf(temp_path, temp_path_size, "%s.umrk.XXXXXX", destination) >=
        (int)temp_path_size) {
        umrk__set_error(error, error_len, "%s", "account temp path is too long");
        return -1;
    }
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        umrk__set_error(error, error_len, "create %s failed: %s",
                        temp_path, strerror(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(temp_path);
        umrk__set_error(error, error_len, "protect %s failed: %s",
                        temp_path, strerror(saved_errno));
        return -1;
    }
    if (umrk__fchmod_if_supported(fd, mode, error, error_len) != 0) {
        close(fd);
        unlink(temp_path);
        return -1;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        int saved_errno = errno;
        close(fd);
        unlink(temp_path);
        umrk__set_error(error, error_len, "fdopen %s failed: %s",
                        temp_path, strerror(saved_errno));
        return -1;
    }
    *out = fp;
    *fd_out = fd;
    return 0;
}

static int umrk__rewrite_passwd(const umrk_ssh_config *cfg, const char *stale_username,
                                char *error, size_t error_len) {
    FILE *in = NULL;
    FILE *out = NULL;
    char line[4096];
    bool found = false;
    char temp_path[PATH_MAX];
    const char *destination = umrk__passwd_path();
    int output_fd = -1;

    if (umrk__open_text_input(destination, &in, error, error_len) != 0 ||
        umrk__open_account_temp(destination, 0644, temp_path, sizeof(temp_path),
                                &out, &output_fd, error, error_len) != 0) {
        if (in) {
            fclose(in);
        }
        return -1;
    }

    while (fgets(line, sizeof(line), in)) {
        if (umrk__line_has_name(line, cfg->username)) {
            if (fprintf(out, "%s:x:0:0:UMRK SSH Admin:%s:/bin/bash\n",
                        cfg->username, cfg->start_dir) < 0) {
                goto write_failed;
            }
            found = true;
            continue;
        }
        if (stale_username && stale_username[0] &&
            umrk__line_has_name(line, stale_username)) {
            continue;
        }
        if (fputs(line, out) == EOF) {
            goto write_failed;
        }
    }

    if (!found && fprintf(out, "%s:x:0:0:UMRK SSH Admin:%s:/bin/bash\n",
                          cfg->username, cfg->start_dir) < 0) {
        goto write_failed;
    }
    return umrk__finish_account_file(in, out, output_fd, temp_path,
                                     destination, 0644, error, error_len);

write_failed:
    fclose(in);
    fclose(out);
    unlink(temp_path);
    umrk__set_error(error, error_len, "%s", "writing passwd replacement failed");
    return -1;
}

static int umrk__rewrite_shadow(const umrk_ssh_config *cfg, const char *stale_username,
                                const char *hash, char *error, size_t error_len) {
    FILE *in = NULL;
    FILE *out = NULL;
    char line[4096];
    bool found = false;
    char temp_path[PATH_MAX];
    const char *destination = umrk__shadow_path();
    int output_fd = -1;
    long day_count = umrk__shadow_day_count();

    if (umrk__open_text_input(destination, &in, error, error_len) != 0 ||
        umrk__open_account_temp(destination, 0600, temp_path, sizeof(temp_path),
                                &out, &output_fd, error, error_len) != 0) {
        if (in) {
            fclose(in);
        }
        return -1;
    }

    while (fgets(line, sizeof(line), in)) {
        if (umrk__line_has_name(line, cfg->username)) {
            if (fprintf(out, "%s:%s:%ld:0:99999:7:::\n",
                        cfg->username, hash, day_count) < 0) {
                goto write_failed;
            }
            found = true;
            continue;
        }
        if (stale_username && stale_username[0] &&
            umrk__line_has_name(line, stale_username)) {
            continue;
        }
        if (fputs(line, out) == EOF) {
            goto write_failed;
        }
    }

    if (!found && fprintf(out, "%s:%s:%ld:0:99999:7:::\n",
                          cfg->username, hash, day_count) < 0) {
        goto write_failed;
    }
    return umrk__finish_account_file(in, out, output_fd, temp_path,
                                     destination, 0600, error, error_len);

write_failed:
    fclose(in);
    fclose(out);
    unlink(temp_path);
    umrk__set_error(error, error_len, "%s", "writing shadow replacement failed");
    return -1;
}

static bool umrk__passwd_matches(const umrk_ssh_config *cfg,
                                 const char *stale_username) {
    FILE *fp = NULL;
    char line[4096];
    char expected[PATH_MAX + 128];
    int matches = 0;
    if (snprintf(expected, sizeof(expected),
                 "%s:x:0:0:UMRK SSH Admin:%s:/bin/bash\n",
                 cfg->username, cfg->start_dir) >= (int)sizeof(expected) ||
        umrk__open_text_input(umrk__passwd_path(), &fp, NULL, 0) != 0) {
        return false;
    }
    while (fgets(line, sizeof(line), fp)) {
        if (stale_username && stale_username[0] &&
            umrk__line_has_name(line, stale_username)) {
            fclose(fp);
            return false;
        }
        if (umrk__line_has_name(line, cfg->username)) {
            if (strcmp(line, expected) != 0) {
                fclose(fp);
                return false;
            }
            matches++;
        }
    }
    bool valid = !ferror(fp) && matches == 1;
    fclose(fp);
    return valid;
}

static bool umrk__shadow_matches(const umrk_ssh_config *cfg,
                                 const char *stale_username,
                                 const char *hash) {
    FILE *fp = NULL;
    char line[4096];
    char prefix[640];
    int matches = 0;
    if (snprintf(prefix, sizeof(prefix), "%s:%s:", cfg->username, hash) >=
        (int)sizeof(prefix) ||
        umrk__open_text_input(umrk__shadow_path(), &fp, NULL, 0) != 0) {
        return false;
    }
    while (fgets(line, sizeof(line), fp)) {
        if (stale_username && stale_username[0] &&
            umrk__line_has_name(line, stale_username)) {
            fclose(fp);
            return false;
        }
        if (umrk__line_has_name(line, cfg->username)) {
            if (strncmp(line, prefix, strlen(prefix)) != 0) {
                fclose(fp);
                return false;
            }
            matches++;
        }
    }
    bool valid = !ferror(fp) && matches == 1;
    fclose(fp);
    return valid;
}

int umrk_ssh_apply_account(const umrk_ssh_config *cfg, const umrk_ssh_paths *paths,
                           char *status, size_t status_len) {
    char hash[512];
    const char *stale_username = NULL;

    if (!cfg || !paths) {
        umrk__set_error(status, status_len, "%s", "missing account apply input");
        return -1;
    }

    if (umrk__validate_username(cfg->username, status, status_len) != 0 ||
        (cfg->password_auth_enabled &&
         (!cfg->password_configured ||
          umrk__validate_password_hash(cfg->password_hash, status, status_len) != 0)) ||
        umrk__validate_start_dir(cfg->start_dir, status, status_len) != 0) {
        return -1;
    }

    if (umrk_ssh_ensure_dir(paths->state_root, 0755, status, status_len) != 0) {
        return -1;
    }

    if (umrk_ssh_ensure_dir(cfg->start_dir, 0755, status, status_len) != 0) {
        return -1;
    }
    if (umrk__install_authorized_keys(cfg, paths, status, status_len) != 0) {
        return -1;
    }

    if (cfg->last_applied_username[0] && strcmp(cfg->last_applied_username, cfg->username) != 0) {
        stale_username = cfg->last_applied_username;
    }

    snprintf(hash, sizeof(hash), "%s",
             cfg->password_auth_enabled ? cfg->password_hash : "!");

    if (umrk__passwd_matches(cfg, stale_username) &&
        umrk__shadow_matches(cfg, stale_username, hash)) {
        umrk__wipe(hash, sizeof(hash));
        if (umrk__remove_legacy_persistent_backups(paths,
                                                   status, status_len) != 0) {
            return -1;
        }
        snprintf(status, status_len, "Account %s already current", cfg->username);
        return 0;
    }

    if (umrk__ensure_rootfs_writable(status, status_len) != 0) {
        umrk__wipe(hash, sizeof(hash));
        return -1;
    }

    /* Each rootfs file is constructed, fsynced, and renamed atomically in
       /etc. Publish shadow first: a power loss between the two renames leaves
       at worst an inert/unreachable hash or a temporarily old passwd row, and
       the next supervised start deterministically converges both files. */
    if (umrk__rewrite_shadow(cfg, stale_username, hash,
                             status, status_len) != 0 ||
        umrk__rewrite_passwd(cfg, stale_username,
                             status, status_len) != 0) {
        umrk__wipe(hash, sizeof(hash));
        return -1;
    }

    umrk__wipe(hash, sizeof(hash));
    if (umrk__remove_legacy_persistent_backups(paths, status, status_len) != 0) {
        return -1;
    }
    snprintf(status, status_len, "Applied account %s as UID 0 alias", cfg->username);
    return 0;
}
