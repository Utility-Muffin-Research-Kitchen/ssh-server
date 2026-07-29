#define _GNU_SOURCE
#include "account.h"
#include "config.h"

#include <assert.h>
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

    puts("PASS config-test legacy plaintext is replaced by a synced hash-only config");
    return 0;
}
