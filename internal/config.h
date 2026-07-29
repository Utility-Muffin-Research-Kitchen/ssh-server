#ifndef UMRK_SSH_CONFIG_H
#define UMRK_SSH_CONFIG_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define UMRK_SSH_DEFAULT_PORT 2222
#define UMRK_SSH_DEFAULT_USERNAME "sshadmin"
#define UMRK_SSH_DEFAULT_BIND "0.0.0.0:2222"
#define UMRK_SSH_DEFAULT_START_DIR ""

typedef struct {
    char username[64];
    char password_hash[512];
    char legacy_password[128];
    bool password_configured;
    bool password_auth_enabled;
    char bind_address[128];
    char start_dir[PATH_MAX];
    char last_applied_username[64];
} umrk_ssh_config;

typedef struct {
    char app_root[PATH_MAX];
    char state_root[PATH_MAX];
    char config_path[PATH_MAX];
    char hostkeys_dir[PATH_MAX];
    char log_dir[PATH_MAX];
    char app_log_path[PATH_MAX];
    char bundled_dropbear[PATH_MAX];
    char bundled_dropbearkey[PATH_MAX];
    char authorized_keys_path[PATH_MAX];
} umrk_ssh_paths;

void umrk_ssh_config_set_defaults(umrk_ssh_config *cfg);
int  umrk_ssh_paths_init(umrk_ssh_paths *paths, char *error, size_t error_len);
int  umrk_ssh_config_load(const umrk_ssh_paths *paths, umrk_ssh_config *cfg,
                          char *error, size_t error_len);
int  umrk_ssh_config_save(const umrk_ssh_paths *paths, const umrk_ssh_config *cfg,
                          char *error, size_t error_len);
int  umrk_ssh_parse_port_text(const char *text, int *port_out, char *error, size_t error_len);
int  umrk_ssh_config_get_port(const umrk_ssh_config *cfg, int *port_out, char *error, size_t error_len);
int  umrk_ssh_config_set_port(umrk_ssh_config *cfg, int port, char *error, size_t error_len);
int  umrk_ssh_ensure_dir(const char *path, mode_t mode, char *error, size_t error_len);

#endif
