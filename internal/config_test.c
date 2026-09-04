#define _GNU_SOURCE
#include "account.h"
#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_legacy_config(const char *path, const char *password) {
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    assert(fprintf(fp,
                   "username=sshadmin\n"
                   "password=%s\n"
                   "bind_address=0.0.0.0:2222\n"
                   "start_dir=/tmp\n"
                   "last_applied_username=\n",
                   password) > 0);
    assert(fclose(fp) == 0);
}

static void read_file(const char *path, char *out, size_t out_len) {
    FILE *fp = fopen(path, "r");
    assert(fp != NULL);
    size_t amount = fread(out, 1, out_len - 1u, fp);
    assert(!ferror(fp));
    out[amount] = '\0';
    assert(fclose(fp) == 0);
}

static void write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    assert(fputs(text, fp) != EOF);
    assert(fclose(fp) == 0);
}

static void join_path(char *out, size_t out_len,
                      const char *base, const char *name) {
    assert(snprintf(out, out_len, "%s/%s", base, name) < (int)out_len);
}

static void test_weak_password_is_removed(const char *app_root) {
    char temp[] = "/tmp/umrk-ssh-weak-test.XXXXXX";
    char *state_root = mkdtemp(temp);
    char text[8192];
    char error[512] = {0};
    umrk_ssh_paths paths;
    umrk_ssh_config config;

    assert(state_root != NULL);
    assert(setenv("UMRK_SSH_STATE_DIR", state_root, 1) == 0);
    assert(setenv("UMRK_SSH_APP_ROOT", app_root, 1) == 0);
    assert(umrk_ssh_paths_init(&paths, error, sizeof(error)) == 0);
    write_legacy_config(paths.config_path, "short");
    assert(umrk_ssh_config_load(&paths, &config, error, sizeof(error)) == 0);
    assert(umrk_ssh_migrate_legacy_password(&paths, &config,
                                            error, sizeof(error)) == 0);
    assert(!config.password_configured);
    assert(config.password_hash[0] == '\0');
    assert(config.legacy_password[0] == '\0');
    read_file(paths.config_path, text, sizeof(text));
    assert(strstr(text, "short") == NULL);
    assert(strstr(text, "\npassword=") == NULL);
    assert(strstr(text, "password_configured=false") != NULL);

    memset(&config, 0, sizeof(config));
    assert(umrk_ssh_configure_password(&config, "elevenchars",
                                       error, sizeof(error)) != 0);
    assert(strstr(error, "at least 12") != NULL);
}

static void test_uncredentialed_defaults_fail_with_message(const char *app_root) {
    char temp[] = "/tmp/umrk-ssh-unconfigured-test.XXXXXX";
    char *root = mkdtemp(temp);
    char state_root[4096];
    char start_dir[4096];
    char error[512] = {0};
    umrk_ssh_paths paths;
    umrk_ssh_config config;

    assert(root != NULL);
    join_path(state_root, sizeof(state_root), root, "state");
    join_path(start_dir, sizeof(start_dir), root, "start");
    assert(setenv("UMRK_SSH_STATE_DIR", state_root, 1) == 0);
    assert(setenv("UMRK_SSH_APP_ROOT", app_root, 1) == 0);
    assert(umrk_ssh_paths_init(&paths, error, sizeof(error)) == 0);

    /* Fresh install: defaults enable password auth with no password set.
       apply_account must reject with a clear message, not the empty status
       the short-circuit previously produced. */
    umrk_ssh_config_set_defaults(&config);
    assert(snprintf(config.start_dir, sizeof(config.start_dir), "%s", start_dir) <
           (int)sizeof(config.start_dir));
    assert(umrk_ssh_apply_account(&config, &paths, error, sizeof(error)) != 0);
    assert(error[0] != '\0');
    assert(strstr(error, "password") != NULL);
}

static void test_account_publish_and_authorized_keys(const char *app_root) {
    char temp[] = "/tmp/umrk-ssh-account-test.XXXXXX";
    char *root = mkdtemp(temp);
    char state_root[4096];
    char start_dir[4096];
    char passwd_path[4096];
    char shadow_path[4096];
    char ssh_dir[4096];
    char destination[4096];
    char victim[4096];
    char text[8192];
    char error[512] = {0};
    struct stat before_passwd;
    struct stat before_shadow;
    struct stat after;
    umrk_ssh_paths paths;
    umrk_ssh_config config;

    assert(root != NULL);
    join_path(state_root, sizeof(state_root), root, "state");
    join_path(start_dir, sizeof(start_dir), root, "start");
    join_path(passwd_path, sizeof(passwd_path), root, "passwd");
    join_path(shadow_path, sizeof(shadow_path), root, "shadow");
    assert(mkdir(state_root, 0700) == 0);
    assert(mkdir(start_dir, 0700) == 0);
    write_text(passwd_path, "root:x:0:0:root:/root:/bin/sh\n");
    write_text(shadow_path, "root:!:0:0:99999:7:::\n");

    assert(setenv("UMRK_SSH_STATE_DIR", state_root, 1) == 0);
    assert(setenv("UMRK_SSH_APP_ROOT", app_root, 1) == 0);
    assert(setenv("UMRK_SSH_TEST_PASSWD_PATH", passwd_path, 1) == 0);
    assert(setenv("UMRK_SSH_TEST_SHADOW_PATH", shadow_path, 1) == 0);
    assert(umrk_ssh_paths_init(&paths, error, sizeof(error)) == 0);
    umrk_ssh_config_set_defaults(&config);
    assert(snprintf(config.start_dir, sizeof(config.start_dir), "%s", start_dir) <
           (int)sizeof(config.start_dir));
    assert(umrk_ssh_configure_password(&config, "correct-horse-battery",
                                       error, sizeof(error)) == 0);
    assert(snprintf(config.password_hash, sizeof(config.password_hash), "%s",
                    "$6$fixture$0123456789abcdef0123456789abcdef") <
           (int)sizeof(config.password_hash));
    if (umrk_ssh_apply_account(&config, &paths, error, sizeof(error)) != 0) {
        fprintf(stderr, "first account apply failed: %s\n", error);
        abort();
    }
    read_file(passwd_path, text, sizeof(text));
    assert(strstr(text, "sshadmin:x:0:0:UMRK SSH Admin:") != NULL);
    read_file(shadow_path, text, sizeof(text));
    assert(strstr(text, "sshadmin:$6$") != NULL);

    assert(stat(passwd_path, &before_passwd) == 0);
    assert(stat(shadow_path, &before_shadow) == 0);
    if (umrk_ssh_apply_account(&config, &paths, error, sizeof(error)) != 0) {
        fprintf(stderr, "idempotent account apply failed: %s\n", error);
        abort();
    }
    assert(stat(passwd_path, &after) == 0);
    assert(after.st_ino == before_passwd.st_ino);
    assert(stat(shadow_path, &after) == 0);
    assert(after.st_ino == before_shadow.st_ino);

    write_text(paths.authorized_keys_path,
               "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAITest fixture\n");
    join_path(ssh_dir, sizeof(ssh_dir), start_dir, ".ssh");
    join_path(destination, sizeof(destination), ssh_dir, "authorized_keys");
    join_path(victim, sizeof(victim), root, "victim");
    assert(mkdir(ssh_dir, 0700) == 0);
    write_text(victim, "must-not-change\n");
    assert(symlink(victim, destination) == 0);
    config.password_auth_enabled = false;
    if (umrk_ssh_apply_account(&config, &paths, error, sizeof(error)) != 0) {
        fprintf(stderr, "key-only account apply failed: %s\n", error);
        abort();
    }
    read_file(victim, text, sizeof(text));
    assert(strcmp(text, "must-not-change\n") == 0);
    assert(lstat(destination, &after) == 0 && S_ISREG(after.st_mode));
    read_file(destination, text, sizeof(text));
    assert(strstr(text, "ssh-ed25519 ") == text);
}

int main(void) {
    char temp[] = "/tmp/umrk-ssh-config-test.XXXXXX";
    char *state_root = mkdtemp(temp);
    char app_root[4096];
    char config_text[8192];
    char backup_path[4096];
    char error[512] = {0};
    const char *legacy_password = "plain-text-must-disappear";
    umrk_ssh_paths paths;
    umrk_ssh_config config;

    assert(state_root != NULL);
    assert(getcwd(app_root, sizeof(app_root)) != NULL);
    assert(setenv("UMRK_SSH_STATE_DIR", state_root, 1) == 0);
    assert(setenv("UMRK_SSH_APP_ROOT", app_root, 1) == 0);
    assert(umrk_ssh_paths_init(&paths, error, sizeof(error)) == 0);
    write_legacy_config(paths.config_path, legacy_password);

    assert(umrk_ssh_config_load(&paths, &config, error, sizeof(error)) == 0);
    assert(strcmp(config.legacy_password, legacy_password) == 0);
    assert(!config.password_configured);
    assert(umrk_ssh_migrate_legacy_password(&paths, &config,
                                            error, sizeof(error)) == 0);
    assert(config.legacy_password[0] == '\0');
    assert(config.password_configured);
    assert(config.password_hash[0] == '$');

    read_file(paths.config_path, config_text, sizeof(config_text));
    assert(strstr(config_text, legacy_password) == NULL);
    assert(strstr(config_text, "\npassword=") == NULL);
    assert(strstr(config_text, "password_hash=$") != NULL);
    assert(strstr(config_text, "password_configured=true") != NULL);
    assert(snprintf(backup_path, sizeof(backup_path), "%s.bak", paths.config_path) < (int)sizeof(backup_path));
    assert(access(backup_path, F_OK) != 0);

    struct stat st;
    assert(stat(paths.config_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    test_weak_password_is_removed(app_root);
    test_uncredentialed_defaults_fail_with_message(app_root);
    test_account_publish_and_authorized_keys(app_root);

    puts("PASS config-test hash migration, password floor, atomic account publish, and safe authorized_keys");
    return 0;
}
